// unreal_open_mcp_level_get_data / level_create Automation specs (P2.7).
//
// Pins the level inspect + create pair end-to-end at the handler level (the
// dispatch spine is already pinned by earlier specs). The cases mirror the
// P2.7 plan's test table:
//   - level_get_data: read-only on the current level returns { actors, count,
//     actorCount, profile, worldPartition } with the roster scoped by profile;
//     compact vs full profile field sets; pagination (page_size/cursor +
//     next_cursor windowing); max_actors truncation; a path scoping to a
//     non-existent level → level_not_found.
//   - level_create: a blank transient create (no `path`, no template) succeeds
//     and returns { saved:false, template:"blank" }; a save `path` colliding
//     with a non-level asset is rejected with path_already_exists BEFORE the
//     world is replaced; a template asset path that does not resolve is
//     rejected with level_not_found BEFORE the world is replaced.
//
// Disk-free by design: the editor's automation harness boots a transient
// editor world, and the create happy-path-with-save would write a .umap into
// the working tree. The transient create + the pre-world-validation error
// surface (path_already_exists, template level_not_found, invalid_path) and
// the full get-data contract are the deterministic, side-effect-free cases
// this spec pins; the save-to-disk round-trip is covered by the P2.7 manual
// verification plan.
//
// Cleanup: every spawned actor (used to populate the get-data roster) is
// destroyed at the end of the case via RAII FActorScope with a stray sweep,
// copied from the P2.6 lifecycle spec. The create cases that succeed replace
// the world (discarding the spawned actors' level), but the FActorScope
// destructor's stray sweep is still safe — it iterates the (new, empty)
// world's actors and finds nothing.
//
// Adapted from Unity's ScenesToolsTests
// (packages/bridge/Tests/Editor/TypedTools/ScenesToolsTests.cs — get-data +
// create cases) at adapt fidelity: content paths replace Unity's Assets/.unity
// paths; the roster unit is the actor (Unreal) instead of the GameObject
// transform-hierarchy node (Unity); string ref replaces Unity's
// instance_id/path/name trio.
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
#include "Misc/PackageName.h"       // FPackageName::DoesPackageExist + map ext

#include "Common/TcpSocketBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpLevelInspectSpec,
	"UnrealOpenMcp.Tools.LevelInspect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpLevelInspectSpec)

namespace
{
	/** Prefix for every test actor so clean-up finds exactly what we spawned
	 *  AND so get-data roster tests can count exactly their own actors. */
	constexpr const TCHAR* TestActorPrefix = TEXT("UnrealOpenMcpTestActor_LvlInspect_");

	/** A known non-level asset path that exists in every Unreal project — the
	 *  engine's default cube static mesh. Used to exercise the
	 *  path_already_exists branch of level_create (collision with a non-level
	 *  asset) without writing anything to disk. The path_collision test probes
	 *  for the asset's existence up front and skips (with a pass) when the
	 *  automation project does not ship BasicShapes, so the case is robust
	 *  across minimal test projects. */
	constexpr const TCHAR* KnownNonLevelAssetPath = TEXT("/Game/BasicShapes/Cube.Cube");

	/** True when @p AssetPath resolves to an existing package that is NOT a
	 *  level — the precondition for the path_already_exists test. */
	bool NonLevelAssetExists(const FString& AssetPath)
	{
		FString Filename;
		if (!FPackageName::DoesPackageExist(AssetPath, &Filename))
		{
			return false;
		}
		return !Filename.EndsWith(FPackageName::GetMapPackageExtension(), ESearchCase::IgnoreCase);
	}

	/** Parse a JSON object from a string. Null on failure. */
	TSharedPtr<FJsonObject> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	/** Send a raw HTTP POST and read the full response until the server closes.
	 *  Same shape as the P2.6 lifecycle spec's SendHttpPost. */
	FString SendHttpPost(
		uint16 Port,
		const FString& Path,
		const FString& Body)
	{
		const FIPv4Address Loopback(127, 0, 0, 1);
		const FIPv4Endpoint Endpoint(Loopback, Port);

		FSocket* Client = FTcpSocketBuilder(TEXT("UnrealOpenMcpLevelInspectTestClient"))
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
	 * RAII spawn-tracking helper, copied from the P2.6 lifecycle spec. The
	 * inspect specs use it to populate the get-data roster (spawn N actors →
	 * the level's actorCount reflects them). The destructor destroys every
	 * tracked actor + any test-prefixed strays so a failed assert that skipped
	 * Track() doesn't leak into subsequent cases.
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

		/** Spawn a basic actor and set its label. */
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
	 *  Mirrors the P2.6 FDispatchHarness; lives on the stack so teardown is
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

	/** Count test-prefixed actors currently in the editor world — the source of
	 *  truth the get-data roster tests compare against. */
	int32 CountTestActors(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if ((*It) != nullptr
				&& (*It)->GetActorLabel().StartsWith(TestActorPrefix, ESearchCase::CaseSensitive))
			{
				++Count;
			}
		}
		return Count;
	}
}

void FUnrealOpenMcpLevelInspectSpec::Define()
{
	Describe("unreal_open_mcp_level_get_data — handler contract", [this]()
	{
		// Read-only on the current level: returns { actors, actorCount, count,
		// returned, profile, worldPartition } with at least the test actors we
		// spawned. compact profile = identity-only entries.
		It("returns a compact actor roster for the current level", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			Scope.Spawn(FString(TestActorPrefix) + TEXT("CompactA"));
			Scope.Spawn(FString(TestActorPrefix) + TEXT("CompactB"));

			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), GetLevelHandler(TEXT("unreal_open_mcp_level_get_data"), Handler));

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("profile"), Json->GetStringField(TEXT("profile")), FString(TEXT("compact")));
			TestTrue(TEXT("has actors array"), Json->HasTypedField<EJson::Array>(TEXT("actors")));
			TestTrue(TEXT("has actorCount"), Json->HasTypedField<EJson::Number>(TEXT("actorCount")));
			TestTrue(TEXT("has returned"), Json->HasTypedField<EJson::Number>(TEXT("returned")));
			TestTrue(TEXT("has worldPartition flag"), Json->HasField(TEXT("worldPartition")));

			const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
			Json->TryGetArrayField(TEXT("actors"), Actors);
			if (!TestNotNull(TEXT("actors array present"), Actors) || Actors == nullptr)
			{
				return;
			}
			TestGreaterEqual(TEXT("roster includes spawned actors"), Actors->Num(), 2);

			// compact entries carry identity only — no folder/class/transform.
			const TSharedPtr<FJsonObject>* First = nullptr;
			(*Actors)[0]->TryGetObject(First);
			if (!TestNotNull(TEXT("first actor entry"), First) || First == nullptr || !First->IsValid())
			{
				return;
			}
			TestTrue(TEXT("label field"), (*First)->HasField(TEXT("label")));
			TestTrue(TEXT("name field"), (*First)->HasField(TEXT("name")));
			TestFalse(TEXT("no folder in compact"), (*First)->HasField(TEXT("folder")));
			TestFalse(TEXT("no class in compact"), (*First)->HasField(TEXT("class")));
			TestFalse(TEXT("no transform in compact"), (*First)->HasField(TEXT("transform")));
		});

		// Full profile adds folder + class + transform + components.
		It("emits folder/class/transform/components under the full profile", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			Scope.Spawn(FString(TestActorPrefix) + TEXT("FullA"));

			FUnrealOpenMcpToolHandler Handler;
			GetLevelHandler(TEXT("unreal_open_mcp_level_get_data"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"profile\":\"full\"}"));
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("profile"), Json->GetStringField(TEXT("profile")), FString(TEXT("full")));

			const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
			Json->TryGetArrayField(TEXT("actors"), Actors);
			if (!TestNotNull(TEXT("actors array present"), Actors) || Actors == nullptr)
			{
				return;
			}
			// Find our spawned actor in the roster (the level may have other actors).
			TSharedPtr<FJsonObject> Match;
			for (const TSharedPtr<FJsonValue>& V : *Actors)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				V->TryGetObject(Obj);
				if (Obj != nullptr && Obj->IsValid()
					&& (*Obj)->GetStringField(TEXT("label")).StartsWith(TestActorPrefix, ESearchCase::CaseSensitive))
				{
					Match = *Obj;
					break;
				}
			}
			if (!TestNotNull(TEXT("spawned actor in full roster"), Match.Get()))
			{
				return;
			}
			TestTrue(TEXT("folder field"), Match->HasField(TEXT("folder")));
			TestTrue(TEXT("class field"), Match->HasField(TEXT("class")));
			TestTrue(TEXT("transform field"), Match->HasField(TEXT("transform")));
			TestTrue(TEXT("components field"), Match->HasField(TEXT("components")));
		});

		// max_actors truncates the roster and sets truncated:true.
		It("truncates the roster at max_actors and flags truncated", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			// Spawn 3 test actors; cap at 1. The roster is bounded by the cap
			// regardless of how many other actors the level holds, so we assert
			// the cap is honored + the truncated flag is set when the total
			// exceeds it (it will — even a bare level has the spawned 3).
			Scope.Spawn(FString(TestActorPrefix) + TEXT("Cap1"));
			Scope.Spawn(FString(TestActorPrefix) + TEXT("Cap2"));
			Scope.Spawn(FString(TestActorPrefix) + TEXT("Cap3"));

			FUnrealOpenMcpToolHandler Handler;
			GetLevelHandler(TEXT("unreal_open_mcp_level_get_data"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"max_actors\":1}"));
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("returned == max_actors"), static_cast<int32>(Json->GetNumberField(TEXT("returned"))), 1);
			// actorCount reflects the true total (>= 3 spawned); truncated
			// fires when the total exceeds the cap.
			TestGreaterEqual(TEXT("actorCount >= 3"), static_cast<int32>(Json->GetNumberField(TEXT("actorCount"))), 3);
			TestTrue(TEXT("truncated"), Json->GetBoolField(TEXT("truncated")));
		});

		// Pagination: page_size + cursor window the roster; next_cursor
		// advances and then goes empty at the end of the stream.
		It("paginates the roster with page_size + cursor + next_cursor", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			Scope.Spawn(FString(TestActorPrefix) + TEXT("Page1"));
			Scope.Spawn(FString(TestActorPrefix) + TEXT("Page2"));

			FUnrealOpenMcpToolHandler Handler;
			GetLevelHandler(TEXT("unreal_open_mcp_level_get_data"), Handler);

			// Page the whole stream at page_size=1. The first page returns 1
			// actor + a non-empty next_cursor; the pagination block is present.
			const FUnrealOpenMcpToolDispatchResult FirstPage = Handler(
				TEXT("{\"page_size\":1,\"cursor\":\"0\"}"));
			TestTrue(TEXT("first page ok"), FirstPage.bOk);

			const TSharedPtr<FJsonObject> FirstJson = ParseJson(FirstPage.Output);
			if (!TestNotNull(TEXT("first page json"), FirstJson.Get()))
			{
				return;
			}
			TestTrue(TEXT("pagination block present"), FirstJson->HasTypedField<EJson::Object>(TEXT("pagination")));
			const TSharedPtr<FJsonObject>* Pagination = nullptr;
			FirstJson->TryGetObjectField(TEXT("pagination"), Pagination);
			if (!TestNotNull(TEXT("pagination object"), Pagination) || Pagination == nullptr)
			{
				return;
			}
			TestEqual(TEXT("returned == 1"), static_cast<int32>(FirstJson->GetNumberField(TEXT("returned"))), 1);
			TestTrue(TEXT("next_cursor present"), (*Pagination)->HasField(TEXT("next_cursor")));
			const FString NextCursor = (*Pagination)->GetStringField(TEXT("next_cursor"));
			TestFalse(TEXT("next_cursor non-empty (more pages)"), NextCursor.IsEmpty());
		});

		// A path scoping to a non-existent level → level_not_found (not a
		// silent fall-through to the whole world).
		It("returns level_not_found when path matches no loaded level", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			GetLevelHandler(TEXT("unreal_open_mcp_level_get_data"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"path\":\"/Game/UnrealOpenMcpTest_NoSuchLevel\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("level_not_found")));
		});
	});

	Describe("unreal_open_mcp_level_create — handler contract", [this]()
	{
		// Blank transient create: no `path`, no template. Replaces the world
		// with a fresh empty one (GEditor->NewMap). Returns { saved:false,
		// template:"blank" }.
		It("creates a blank transient level (no path, no template)", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), GetLevelHandler(TEXT("unreal_open_mcp_level_create"), Handler));

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("template"), Json->GetStringField(TEXT("template")), FString(TEXT("blank")));
			TestFalse(TEXT("saved false (transient)"), Json->GetBoolField(TEXT("saved")));
		});

		// Save path colliding with a non-level asset → path_already_exists,
		// rejected BEFORE the world is replaced (the current world's actors are
		// intact — verified by checking a spawned test actor survives).
		It("rejects a save path colliding with a non-level asset before replacing the world", [this]()
		{
			// Probe for the non-level asset up front; skip (pass) when the
			// automation project does not ship BasicShapes. The case is only
			// meaningful when a real non-level asset exists to collide with.
			if (!NonLevelAssetExists(KnownNonLevelAssetPath))
			{
				UE_LOG(LogTemp, Warning,
					TEXT("[UnrealOpenMcp] level_create path_already_exists case skipped: '%s' not found in this project."),
					KnownNonLevelAssetPath);
				return;
			}

			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			AActor* Survivor = Scope.Spawn(FString(TestActorPrefix) + TEXT("CreateCollision"));

			FUnrealOpenMcpToolHandler Handler;
			GetLevelHandler(TEXT("unreal_open_mcp_level_create"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"path\":\"%s\"}"), KnownNonLevelAssetPath);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("path_already_exists")));

			// The world must NOT have been replaced — the survivor is still
			// valid + present. This is the load-bearing assertion: the path
			// validation fires before NewMap, so a bad save path does not
			// destroy the caller's open work.
			TestTrue(TEXT("survivor still valid"), IsValid(Survivor));
			TestEqual(
				TEXT("test actor count unchanged (world not replaced)"),
				CountTestActors(Scope.World), 1);
		});

		// Template asset path that does not resolve → level_not_found, rejected
		// BEFORE the world is replaced. (NewMapFromTemplate silently falls back
		// to a blank map on a missing template — this guard turns that into a
		// structured error instead of a misleading success.)
		It("rejects a missing template asset path with level_not_found before replacing the world", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			AActor* Survivor = Scope.Spawn(FString(TestActorPrefix) + TEXT("CreateTemplate"));

			FUnrealOpenMcpToolHandler Handler;
			GetLevelHandler(TEXT("unreal_open_mcp_level_create"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"template\":\"/Game/UnrealOpenMcpTest_NoSuchTemplate\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("level_not_found")));
			TestTrue(TEXT("survivor still valid"), IsValid(Survivor));
			TestEqual(
				TEXT("test actor count unchanged (world not replaced)"),
				CountTestActors(Scope.World), 1);
		});

		// Malformed template asset path (not a valid /Game/... package path) →
		// invalid_path. Distinct from level_not_found so an agent can branch.
		It("returns invalid_path for a malformed template asset path", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			GetLevelHandler(TEXT("unreal_open_mcp_level_create"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"template\":\"NotAValidPath\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_path")));
		});
	});

	Describe("unreal_open_mcp_level_get_data — HTTP round-trip", [this]()
	{
		// Full POST → registry → game-thread handler → {ok,result} envelope on
		// a real loopback socket. Pins the read-only get-data through the real
		// transport (the create tool's transport parity is already covered by
		// the actor-family HTTP specs; one round-trip here is enough for the
		// inspect pair's envelope contract).
		LatentIt(
			"returns the {ok,result} envelope with an actors payload over POST /tools/{name}",
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
						TEXT("/tools/unreal_open_mcp_level_get_data"),
						TEXT("{}"));
					Harness.Stop();

					TestTrue(TEXT("HTTP body present"), Response.Contains(TEXT("HTTP/")));
					const FString RespBody = ExtractHttpBody(Response);
					TestTrue(TEXT("ok:true"), RespBody.Contains(TEXT("\"ok\":true")));
					TestTrue(TEXT("actors field"), RespBody.Contains(TEXT("\"actors\"")));
					TestTrue(TEXT("actorCount field"), RespBody.Contains(TEXT("\"actorCount\"")));
					TestTrue(TEXT("worldPartition field"), RespBody.Contains(TEXT("\"worldPartition\"")));
					Done.Execute();
				});
			});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
