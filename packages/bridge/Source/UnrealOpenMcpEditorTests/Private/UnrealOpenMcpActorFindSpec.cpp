// unreal_open_mcp_actor_find Automation specs (P2.2).
//
// Pins the first real read-only typed tool end-to-end. Three surfaces:
//   1. Handler contract (no HTTP): targeted resolve (hit + miss), list mode
//      with filters, structured errors (no world / invalid body / bad class),
//      and the ActorData shape. These call the registered handler directly via
//      a fresh FUnrealOpenMcpToolRegistry so the actor resolution + serializer
//      logic is unit-pinned independent of the HTTP transport.
//   2. HTTP round-trip: POST /tools/unreal_open_mcp_actor_find through a real
//      loopback socket against an FDispatchHarness, asserting the {ok,result}
//      envelope carries the actors payload. This makes actor_find a valid
//      P2.8 smoke candidate.
//   3. No-editor-world path: when the handler cannot reach a world it returns
//      {ok:false, error:{code:"no_editor_world"}}. (In the editor test runner
//      a world is normally present, so this is covered by a body-validation
//      case that fails before world resolution.)
//
// Test actors are spawned into the editor world and cleaned up at the end of
// each case (RAII TArray<AActor*> + DestroyActor). Names are prefixed
// "UnrealOpenMcpTestActor_" so the name_contains filter is deterministic.
//
// Adapted from Unity's GameObjectsToolsTests
// (packages/bridge/Tests/Editor/TypedTools/GameObjectsToolsTests.cs — Find
// cases) at adapt fidelity: string `actor` ref replaces instance_id; pitch/
// yaw/roll replaces euler x/y/z; targeted-miss returns {ok:true,notFound:true}.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Bridge/UnrealOpenMcpBridgeEnvelope.h"
#include "Bridge/UnrealOpenMcpBridgeHttpServer.h"
#include "Bridge/UnrealOpenMcpBridgeRequestQueue.h"
#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Dispatch/UnrealOpenMcpGameThreadDispatcher.h"
#include "Tools/UnrealOpenMcpActorTools.h"
#include "Tools/UnrealOpenMcpObjectRef.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "EngineUtils.h"            // TActorIterator
#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include "Common/TcpSocketBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpActorFindSpec,
	"UnrealOpenMcp.Tools.ActorFind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpActorFindSpec)

namespace
{
	/** Prefix for every test actor so name_contains filters are deterministic
	 *  and clean-up finds exactly what we spawned. */
	constexpr const TCHAR* TestActorPrefix = TEXT("UnrealOpenMcpTestActor_");

	/** Parse a JSON object from a string. Null on failure. */
	TSharedPtr<FJsonObject> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	/** Send a raw HTTP POST and read the full response until the server closes.
	 *  Same shape as the P2.1 dispatch spec's SendHttp. */
	FString SendHttpPost(
		uint16 Port,
		const FString& Path,
		const FString& Body)
	{
		const FIPv4Address Loopback(127, 0, 0, 1);
		const FIPv4Endpoint Endpoint(Loopback, Port);

		FSocket* Client = FTcpSocketBuilder(TEXT("UnrealOpenMcpActorFindTestClient"))
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

	/** RAII spawn/destroy helper. Spawns an AActor with the given label into the
	 *  editor world; destroys it (and any test-prefixed strays) on destruction
	 *  so a failed assert never leaks actors into the level. */
	struct FActorScope
	{
		TArray<AActor*> Spawned;
		UWorld* World = nullptr;

		~FActorScope()
		{
			for (AActor* Actor : Spawned)
			{
				if (Actor != nullptr && IsValid(Actor))
				{
					World->DestroyActor(Actor, true);
				}
			}
		}

		/** Spawn a basic actor and set its label. Returns null if no world. */
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

	/** Ephemeral dispatch harness wired with the actor tools registered.
	 *  Mirrors the P2.1 FDispatchHarness; lives on the stack so teardown is
	 *  server-first. */
	struct FDispatchHarness
	{
		FUnrealOpenMcpGameThreadDispatcher Dispatcher;
		FUnrealOpenMcpToolRegistry Registry;
		FUnrealOpenMcpBridgeRequestQueue Queue;
		FUnrealOpenMcpBridgeHttpServer* Server = nullptr;

		bool Start(const FString& ProjectPath)
		{
			FUnrealOpenMcpActorTools::Register(Registry);
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
}

void FUnrealOpenMcpActorFindSpec::Define()
{
	Describe("unreal_open_mcp_actor_find — handler contract", [this]()
	{
		// Targeted hit: the `actor` ref resolves to a single actor, the result
		// carries exactly one ActorData with the label, and notFound is absent
		// (or false).
		It("targeted find by label returns the actor", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			Scope.Spawn(FString(TestActorPrefix) + TEXT("Targeted"));

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), Registry.TryGet(TEXT("unreal_open_mcp_actor_find"), Handler));

			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%sTargeted\"}"), TestActorPrefix);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			const TArray<TSharedPtr<FJsonValue>>* Actors;
			TestTrue(TEXT("actors array"), Json->TryGetArrayField(TEXT("actors"), Actors));
			TestEqual(TEXT("one actor"), Actors->Num(), 1);
			if (Actors->Num() == 1)
			{
				const TSharedPtr<FJsonObject> Entry = (*Actors)[0]->AsObject;
				TestEqual(TEXT("label"), Entry->GetStringField(TEXT("label")), FString(TestActorPrefix) + TEXT("Targeted"));
			}
			TestFalse(TEXT("notFound false"), Json->GetBoolField(TEXT("notFound")));
		});

		// Targeted miss: the `actor` ref resolves to nothing → {ok:true,
		// notFound:true, actors:[]}. This is NOT an error (copied from Unity's
		// gameobject-find contract).
		It("targeted find miss returns ok with notFound=true", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_find"), Handler);

			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"actor\":\"UnrealOpenMcpTestActor_DoesNotExist\"}"));
			TestTrue(TEXT("ok (miss is not an error)"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestTrue(TEXT("notFound true"), Json->GetBoolField(TEXT("notFound")));
			const TArray<TSharedPtr<FJsonValue>>* Actors;
			TestTrue(TEXT("actors array"), Json->TryGetArrayField(TEXT("actors"), Actors));
			TestEqual(TEXT("empty actors"), Actors->Num(), 0);
		});

		// List mode: enumerate actors, filtered by name_contains; result is
		// bounded by max_results and carries a truncated count.
		It("list mode with name_contains filter returns bounded results", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			Scope.Spawn(FString(TestActorPrefix) + TEXT("ListA"));
			Scope.Spawn(FString(TestActorPrefix) + TEXT("ListB"));
			Scope.Spawn(FString(TestActorPrefix) + TEXT("ListC"));

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_find"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"name_contains\":\"%sList\",\"max_results\":2}"), TestActorPrefix);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			const TArray<TSharedPtr<FJsonValue>>* Actors;
			TestTrue(TEXT("actors array"), Json->TryGetArrayField(TEXT("actors"), Actors));
			// max_results clamps to 2 → exactly two emitted, truncated >= 1.
			TestEqual(TEXT("max_results honored"), Actors->Num(), 2);
			const int32 Truncated = static_cast<int32>(Json->GetNumberField(TEXT("truncated")));
			TestTrue(TEXT("truncated reports overflow"), Truncated >= 1);
		});

		// Invalid body → structured error (invalid_parameter), not a crash.
		It("returns invalid_parameter for a malformed body", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_find"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("<<<not json>>>"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});

		// Invalid class filter → structured error.
		It("returns invalid_parameter when a class filter does not resolve", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_find"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"class\":\"NotARealClass.UnrealOpenMcp\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});
	});

	Describe("unreal_open_mcp_actor_find — HTTP round-trip", [this]()
	{
		// Full POST → registry → game-thread handler → {ok,result} envelope on
		// a real loopback socket. This is the path P2.8 smoke exercises.
		LatentIt(
			"returns the {ok,result} envelope over POST /tools/{name}",
			FTimespan::FromSeconds(30),
			[this](const FDoneDelegate& Done)
			{
				Async(EAsyncExecution::Thread, [this, Done]()
				{
					FActorScope Scope;
					Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld();
					if (Scope.World != nullptr)
					{
						Scope.Spawn(FString(TestActorPrefix) + TEXT("HTTP"));
					}

					FDispatchHarness Harness;
					if (!TestTrue(TEXT("server started"), Harness.Start(TEXT("/tmp/test"))))
					{
						Done.Execute();
						return;
					}

					const FString Response = SendHttpPost(
						Harness.Server->GetPort(),
						TEXT("/tools/unreal_open_mcp_actor_find"),
						TEXT("{}"));
					Harness.Stop();

					TestTrue(TEXT("HTTP body present"), Response.Contains(TEXT("HTTP/")));
					const FString Body = ExtractHttpBody(Response);
					TestTrue(TEXT("ok:true"), Body.Contains(TEXT("\"ok\":true")));
					TestTrue(TEXT("actors field"), Body.Contains(TEXT("\"actors\"")));
					TestTrue(TEXT("count field"), Body.Contains(TEXT("\"count\"")));
					Done.Execute();
				});
			});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
