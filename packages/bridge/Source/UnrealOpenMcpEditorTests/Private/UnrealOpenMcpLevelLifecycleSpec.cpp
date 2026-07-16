// unreal_open_mcp_level_open / level_save / level_list_loaded /
// level_set_current / level_unload_sublevel Automation specs (P2.6).
//
// Pins the five level lifecycle tools end-to-end at the handler level (the
// dispatch spine is already pinned by earlier specs). The cases mirror the
// P2.6 plan's test table:
//   - level_open: missing path → missing_parameter; invalid path → invalid_path;
//     non-existent /Game/... path → level_not_found; dirty guard blocks a
//     destructive open when the current world has unsaved edits; ignore_dirty
//     bypasses the guard (the call then falls through to level_not_found because
//     the target .umap does not exist — that fall-through IS the proof the
//     guard was bypassed, not blocked).
//   - level_save: a transient/never-saved current level with no `path` →
//     save_failed (the in-place path would otherwise raise a modal Save-As
//     dialog); an invalid save-as `path` → invalid_path.
//   - level_list_loaded: read-only, returns { levels: [...], count } with at
//     least the persistent level, path-first identity ({ path, name, isCurrent,
//     dirty }), plus load/visibility flags.
//   - level_set_current: missing path → missing_parameter; a name that matches
//     no loaded level → level_not_found.
//   - level_unload_sublevel: targeting the persistent level → persistent_level;
//     a name that matches no streaming sublevel → level_not_found.
// Plus one HTTP round-trip for level_list_loaded so the {ok,result} envelope is
// pinned through the real transport.
//
// Disk-free by design: the editor's automation harness boots a transient
// editor world (no .umap on disk), so the happy-path level_open cannot be
// pinned here without polluting the working tree with a test .umap. The
// dirty-guard + ignore_dirty bypass, the error surface, and the read-only list
// are the deterministic, side-effect-free contract this spec pins; the full
// open/save round-trip is covered by the P2.6 manual verification plan.
//
// Cleanup: every spawned actor (used to dirty the package for the guard test)
// is destroyed at the end of the case via RAII FActorScope with a stray sweep,
// copied from the P2.4 modify spec. The dirty-guard cases additionally call
// GEditor->DiscardEditsAtObject-/MarkPackageDirty-cleared via reloading the
// transient world is unnecessary — destroying the spawned actor and a
// GEditor->DiscardCurrentWorld() would itself discard the world, so the cases
// instead rely on the per-case actor cleanup + the automation harness's own
// world teardown between specs.
//
// Adapted from Unity's ScenesToolsTests
// (packages/bridge/Tests/Editor/TypedTools/ScenesToolsTests.cs — open/save/
// list cases) at adapt fidelity: content paths replace Unity's Assets/.unity
// paths; the dirty guard is package-dirty-state (Unreal) instead of per-scene
// isDirty (Unity); string ref replaces Unity's instance_id/path/name trio.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Bridge/UnrealOpenMcpBridgeHttpServer.h"
#include "Bridge/UnrealOpenMcpBridgeRequestQueue.h"
#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Dispatch/UnrealOpenMcpGameThreadDispatcher.h"
#include "Tools/UnrealOpenMcpLevelTools.h"
#include "Tools/UnrealOpenMcpObjectRef.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "EngineUtils.h"            // TActorIterator
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Editor.h"                 // GEditor
#include "Misc/Paths.h"             // FPaths::GetBaseFilename (persistent-level name)

#include "Common/TcpSocketBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpLevelLifecycleSpec,
	"UnrealOpenMcp.Tools.LevelLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpLevelLifecycleSpec)

namespace
{
	/** Prefix for every test actor so clean-up finds exactly what we spawned. */
	constexpr const TCHAR* TestActorPrefix = TEXT("UnrealOpenMcpTestActor_Level_");

	/** Parse a JSON object from a string. Null on failure. */
	TSharedPtr<FJsonObject> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	/** Send a raw HTTP POST and read the full response until the server closes.
	 *  Same shape as the P2.3 actor-create spec's SendHttpPost. */
	FString SendHttpPost(
		uint16 Port,
		const FString& Path,
		const FString& Body)
	{
		const FIPv4Address Loopback(127, 0, 0, 1);
		const FIPv4Endpoint Endpoint(Loopback, Port);

		FSocket* Client = FTcpSocketBuilder(TEXT("UnrealOpenMcpLevelLifecycleTestClient"))
			.AsBlocking()
			.Build();
		if (Client == nullptr)
		{
			return FString();
		}
		Client->Connect(*Endpoint.ToInternetAddr());

		const FTCHARToUTF8 BodyUtf8(*Body);
		FString Request = FString::Printf(
			TEXT("POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"),
			*Path);
		Request += FString::Printf(TEXT("Content-Length: %d\r\n"), BodyUtf8.Length());
		Request += TEXT("Content-Type: application/json; charset=utf-8\r\n\r\n");

		const FTCHARToUTF8 RequestUtf8(*Request);
		int32 Sent = 0;
		Client->Send(reinterpret_cast<const uint8*>(RequestUtf8.Get()), RequestUtf8.Length(), Sent);
		Client->Send(reinterpret_cast<const uint8*>(BodyUtf8.Get()), BodyUtf8.Length(), Sent);

		TArray<uint8> Received;
		Received.SetNumUninitialized(4096);
		FString Response;
		const FTimespan WaitTimeout = FTimespan::FromSeconds(15.0);
		while (Client->Wait(ESocketWaitConditions::WaitForRead, WaitTimeout))
		{
			int32 BytesRead = 0;
			const bool bOk = Client->Recv(Received.GetData(), Received.Num(), BytesRead);
			if (!bOk || BytesRead == 0)
			{
				break;
			}
			Response += FString(UE_PTRDIFF_TO_INT32(BytesRead), reinterpret_cast<const ANSICHAR*>(Received.GetData()));
		}
		Client->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client);
		return Response;
	}

	FString ExtractHttpBody(const FString& Response)
	{
		const int32 Separator = Response.Find(TEXT("\r\n\r\n"));
		return Separator == INDEX_NONE ? Response : Response.Mid(Separator + 4);
	}

	/**
	 * RAII spawn-tracking helper, copied from the P2.4 modify spec. The level
	 * specs use it to dirty the current package (spawn an actor → the level
	 * package is marked dirty) for the dirty-guard cases. The destructor
	 * destroys every tracked actor + any test-prefixed strays so a failed
	 * assert that skipped Track() doesn't leak into subsequent cases.
	 */
	struct FActorScope
	{
		TArray<AActor*> Spawned;
		UWorld* World = nullptr;

		~FActorScope()
		{
			if (World == nullptr)
			{
				World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			}
			for (AActor* Actor : Spawned)
			{
				if (Actor != nullptr && IsValid(Actor))
				{
					World->DestroyActor(Actor, true);
				}
			}
			if (World != nullptr)
			{
				TArray<AActor*> Strays;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (Actor != nullptr
						&& Actor->GetActorLabel().StartsWith(TestActorPrefix, ESearchCase::CaseSensitive))
					{
						Strays.Add(Actor);
					}
				}
				for (AActor* Actor : Strays)
				{
					World->DestroyActor(Actor, true);
				}
			}
		}

		void Track(AActor* Actor)
		{
			if (Actor != nullptr)
			{
				Spawned.Add(Actor);
			}
		}

		/** Spawn a basic actor and set its label. Spawning marks the level
		 *  package dirty — the lever the dirty-guard cases pull. */
		AActor* Spawn(const FString& Label)
		{
			if (World == nullptr)
			{
				World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			}
			if (World == nullptr)
			{
				return nullptr;
			}
			AActor* Actor = World->SpawnActor<AActor>();
			if (Actor != nullptr)
			{
				Actor->SetActorLabel(*Label);
				Spawned.Add(Actor);
			}
			return Actor;
		}
	};

	/** Ephemeral dispatch harness wired with the level tools registered.
	 *  Mirrors the P2.3 FDispatchHarness; lives on the stack so teardown is
	 *  server-first. */
	struct FDispatchHarness
	{
		FUnrealOpenMcpGameThreadDispatcher Dispatcher;
		FUnrealOpenMcpToolRegistry Registry;
		FUnrealOpenMcpBridgeRequestQueue Queue;
		FUnrealOpenMcpBridgeHttpServer* Server = nullptr;

		bool Start(const FString& ProjectPath)
		{
			FUnrealOpenMcpLevelTools::Register(Registry);
			Server = new FUnrealOpenMcpBridgeHttpServer(Dispatcher, Registry, Queue);
			return Server->Start(0, ProjectPath);
		}

		void Stop()
		{
			if (Server != nullptr)
			{
				Server->Stop();
				delete Server;
				Server = nullptr;
			}
		}
	};

	/** Resolve a level tool handler by name from a fresh registry. Returns
	 *  false (and logs via TestTrue) when the handler is missing. */
	bool GetLevelHandler(const FString& ToolName, FUnrealOpenMcpToolHandler& OutHandler)
	{
		FUnrealOpenMcpToolRegistry Registry;
		FUnrealOpenMcpLevelTools::Register(Registry);
		return Registry.TryGet(ToolName, OutHandler);
	}
}

void FUnrealOpenMcpLevelLifecycleSpec::Define()
{
	Describe("unreal_open_mcp_level_open — handler contract", [this]()
	{
		// Missing path → missing_parameter (not a crash, not a silent empty
		// success). The error is structured so an agent can branch.
		It("returns missing_parameter when path is absent", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), GetLevelHandler(TEXT("unreal_open_mcp_level_open"), Handler));

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});

		// Malformed path (not a valid /Game/... package path) → invalid_path.
		// Distinct from level_not_found so an agent can tell a typo'd path
		// from a package that simply does not exist.
		It("returns invalid_path for a malformed path", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			GetLevelHandler(TEXT("unreal_open_mcp_level_open"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"path\":\"NotAValidPath\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_path")));
		});

		// A well-formed /Game/... path that does not exist on disk →
		// level_not_found (the existence probe catches the miss before LoadMap
		// would log an engine Error, which Automation would count as a failure).
		It("returns level_not_found for a non-existent package path", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			GetLevelHandler(TEXT("unreal_open_mcp_level_open"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"path\":\"/Game/UnrealOpenMcpTest_NoSuchMap\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("level_not_found")));
		});

		// Dirty guard: spawning an actor marks the current level package dirty.
		// A subsequent level_open (without ignore_dirty) is refused with
		// level_dirty — the destructive open is blocked, the world is untouched.
		It("blocks a destructive open with level_dirty when the world is dirty", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			// Spawn → marks the level package dirty.
			Scope.Spawn(FString(TestActorPrefix) + TEXT("DirtyGuard"));

			FUnrealOpenMcpToolHandler Handler;
			GetLevelHandler(TEXT("unreal_open_mcp_level_open"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"path\":\"/Game/UnrealOpenMcpTest_NoSuchMap\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			// The guard fires BEFORE path resolution, so the code is level_dirty,
			// not level_not_found — that ordering is the guard's contract.
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("level_dirty")));
		});

		// ignore_dirty bypass: with ignore_dirty=true the guard no longer blocks.
		// The call then falls through to path resolution → level_not_found
		// (the target .umap does not exist). Reaching level_not_found instead of
		// level_dirty IS the proof the bypass worked — the guard did not fire.
		It("lets ignore_dirty bypass the guard (falls through to level_not_found)", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			Scope.Spawn(FString(TestActorPrefix) + TEXT("DirtyBypass"));

			FUnrealOpenMcpToolHandler Handler;
			GetLevelHandler(TEXT("unreal_open_mcp_level_open"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"path\":\"/Game/UnrealOpenMcpTest_NoSuchMap\",\"ignore_dirty\":true}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			// NOT level_dirty (the guard was bypassed) — level_not_found because
			// the path does not resolve. The dirty world is still intact here;
			// the actual discard only happens on a successful LoadMap.
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("level_not_found")));
		});
	});

	Describe("unreal_open_mcp_level_save — handler contract", [this]()
	{
		// In-place save on a transient/never-saved level (the automation harness
		// boots one) → save_failed. Without this guard the in-place path would
		// raise a modal Save-As file dialog that blocks the game thread.
		It("returns save_failed for an in-place save on a never-saved level", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), GetLevelHandler(TEXT("unreal_open_mcp_level_save"), Handler));

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("save_failed")));
		});

		// Invalid save-as path → invalid_path (the target is not a valid
		// /Game/... package path). Validated before SaveMap is ever called.
		It("returns invalid_path for a malformed save-as target", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			GetLevelHandler(TEXT("unreal_open_mcp_level_save"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"path\":\"NotAValidPath\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_path")));
		});
	});

	Describe("unreal_open_mcp_level_list_loaded — handler contract", [this]()
	{
		// Read-only list: returns { levels: [...], count } with at least the
		// persistent level, path-first identity, and the load/visibility flags.
		// The persistent level is always loaded + visible.
		It("lists the persistent level with path-first identity and flags", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), GetLevelHandler(TEXT("unreal_open_mcp_level_list_loaded"), Handler));

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestTrue(TEXT("has levels array"), Json->HasTypedField<EJson::Array>(TEXT("levels")));
			const int32 Count = static_cast<int32>(Json->GetNumberField(TEXT("count")));

			const TArray<TSharedPtr<FJsonValue>>* Levels = nullptr;
			Json->TryGetArrayField(TEXT("levels"), Levels);
			if (!TestNotNull(TEXT("levels array present"), Levels) || Levels == nullptr)
			{
				return;
			}
			TestEqual(TEXT("count matches array length"), Count, Levels->Num());
			TestGreaterEqual(TEXT("at least one level (persistent)"), Levels->Num(), 1);

			// The first entry is the persistent level — always loaded + visible.
			const TSharedPtr<FJsonObject>* First = nullptr;
			(*Levels)[0]->TryGetObject(First);
			if (!TestNotNull(TEXT("first level entry"), First) || First == nullptr || !First->IsValid())
			{
				return;
			}
			TestTrue(TEXT("path field"), (*First)->HasField(TEXT("path")));
			TestTrue(TEXT("name field"), (*First)->HasField(TEXT("name")));
			TestTrue(TEXT("isCurrent field"), (*First)->HasField(TEXT("isCurrent")));
			TestTrue(TEXT("isPersistent true"), (*First)->GetBoolField(TEXT("isPersistent")));
			TestTrue(TEXT("isLoaded true"), (*First)->GetBoolField(TEXT("isLoaded")));
			TestTrue(TEXT("isVisible true"), (*First)->GetBoolField(TEXT("isVisible")));
		});
	});

	Describe("unreal_open_mcp_level_set_current — handler contract", [this]()
	{
		// Missing path → missing_parameter.
		It("returns missing_parameter when path is absent", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), GetLevelHandler(TEXT("unreal_open_mcp_level_set_current"), Handler));

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});

		// A name that matches no loaded level → level_not_found.
		It("returns level_not_found for a name matching no loaded level", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			GetLevelHandler(TEXT("unreal_open_mcp_level_set_current"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"path\":\"UnrealOpenMcpTest_NoSuchLevel\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("level_not_found")));
		});
	});

	Describe("unreal_open_mcp_level_unload_sublevel — handler contract", [this]()
	{
		// Targeting the persistent level → persistent_level (it cannot be
		// unloaded). The automation world has a persistent level we can name.
		It("rejects unloading the persistent level with persistent_level", [this]()
		{
			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (!TestNotNull(TEXT("editor world"), World))
			{
				return;
			}
			if (!TestNotNull(TEXT("persistent level"), World->PersistentLevel))
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), GetLevelHandler(TEXT("unreal_open_mcp_level_unload_sublevel"), Handler));

			// Use the persistent level's own short name so the guard's
			// persistent-level branch fires.
			const FString Body = FString::Printf(
				TEXT("{\"path\":\"%s\"}"),
				*FPaths::GetBaseFilename(World->PersistentLevel->GetOutermost()->GetName()));
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("persistent_level")));
		});

		// A name that matches no streaming sublevel → level_not_found.
		It("returns level_not_found for a name matching no sublevel", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			GetLevelHandler(TEXT("unreal_open_mcp_level_unload_sublevel"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"path\":\"UnrealOpenMcpTest_NoSuchSublevel\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("level_not_found")));
		});
	});

	Describe("unreal_open_mcp_level_list_loaded — HTTP round-trip", [this]()
	{
		// Full POST → registry → game-thread handler → {ok,result} envelope on
		// a real loopback socket. Pins the read-only list through the real
		// transport (the mutating tools' transport parity is already covered by
		// the actor-family HTTP specs; one round-trip here is enough for the
		// level family's envelope contract).
		LatentIt(
			"returns the {ok,result} envelope with a levels payload over POST /tools/{name}",
			FTimespan::FromSeconds(30),
			[this](const FDoneDelegate& Done)
			{
				Async(EAsyncExecution::Thread, [this, Done]()
				{
					FDispatchHarness Harness;
					if (!TestTrue(TEXT("server started"), Harness.Start(TEXT("/tmp/test"))))
					{
						Done.Execute();
						return;
					}

					const FString Response = SendHttpPost(
						Harness.Server->GetPort(),
						TEXT("/tools/unreal_open_mcp_level_list_loaded"),
						TEXT("{}"));
					Harness.Stop();

					TestTrue(TEXT("HTTP body present"), Response.Contains(TEXT("HTTP/")));
					const FString RespBody = ExtractHttpBody(Response);
					TestTrue(TEXT("ok:true"), RespBody.Contains(TEXT("\"ok\":true")));
					TestTrue(TEXT("levels field"), RespBody.Contains(TEXT("\"levels\"")));
					TestTrue(TEXT("count field"), RespBody.Contains(TEXT("\"count\"")));
					Done.Execute();
				});
			});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
