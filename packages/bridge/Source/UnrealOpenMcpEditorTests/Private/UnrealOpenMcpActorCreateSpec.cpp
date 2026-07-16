// unreal_open_mcp_actor_create Automation specs (P2.3).
//
// Pins the first mutating typed tool end-to-end. Three surfaces:
//   1. Handler contract (no HTTP): native-class spawn returns ActorData with a
//      resolvable label; invalid class path → class_not_found; missing classPath
//      → missing_parameter; parent attachment wires the child to the parent's
//      root; abstract/non-Actor class rejection. These call the registered
//      handler directly via a fresh FUnrealOpenMcpToolRegistry so spawn +
//      resolve + serializer logic is unit-pinned independent of HTTP transport.
//   2. HTTP round-trip: POST /tools/unreal_open_mcp_actor_create through a real
//      loopback socket against an FDispatchHarness, asserting the {ok,result}
//      envelope carries the `actor` payload and the spawned actor is real
//      (resolvable via actor_find's resolver).
//   3. Cleanup: every spawned actor (handler or HTTP path) is destroyed at the
//      end of the case (RAII FActorScope) so a failed assert never leaks actors
//      into the level. Names are prefixed "UnrealOpenMcpTestActor_Create_" so
//      they are deterministic and clean-up finds exactly what we spawned.
//
// Adapted from Unity's GameObjectsToolsTests
// (packages/bridge/Tests/Editor/TypedTools/GameObjectsToolsTests.cs — Create +
// parent-attach cases) at adapt fidelity: string `parent` ref replaces Unity's
// parent_path; classPath replaces primitive_type; the parent-attach assertion
// uses GetAttachParentActor() (Unreal) instead of transform.parent (Unity).
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
	FUnrealOpenMcpActorCreateSpec,
	"UnrealOpenMcp.Tools.ActorCreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpActorCreateSpec)

namespace
{
	/** Prefix for every test actor so clean-up finds exactly what we spawned. */
	constexpr const TCHAR* TestActorPrefix = TEXT("UnrealOpenMcpTestActor_Create_");

	/** Parse a JSON object from a string. Null on failure. */
	TSharedPtr<FJsonObject> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	/** Send a raw HTTP POST and read the full response until the server closes.
	 *  Same shape as the P2.2 actor-find spec's SendHttpPost. */
	FString SendHttpPost(
		uint16 Port,
		const FString& Path,
		const FString& Body)
	{
		const FIPv4Address Loopback(127, 0, 0, 1);
		const FIPv4Endpoint Endpoint(Loopback, Port);

		FSocket* Client = FTcpSocketBuilder(TEXT("UnrealOpenMcpActorCreateTestClient"))
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
	 * RAII spawn-tracking helper. actor_create is itself a spawn path, so the
	 * scope cannot own the SpawnActor call — instead handlers/tests register
	 * any spawned actor (created by the tool or by setup) via Track(), and the
	 * destructor destroys them all (plus any test-prefixed strays in the world
	 * so a partial-cleanup leak from a prior run doesn't accumulate).
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
			// Sweep any leftover test-prefixed actors so a failed assert that
			// skipped Track() doesn't leak into subsequent cases.
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

		/** Register an already-spawned actor for destruction on scope exit. */
		void Track(AActor* Actor)
		{
			if (Actor != nullptr)
			{
				Spawned.Add(Actor);
			}
		}

		/** Spawn a basic actor and set its label (setup helper). Returns null
		 *  if no world. */
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
	 *  Mirrors the P2.2 FDispatchHarness; lives on the stack so teardown is
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

void FUnrealOpenMcpActorCreateSpec::Define()
{
	Describe("unreal_open_mcp_actor_create — handler contract", [this]()
	{
		// Native class spawn: a real built-in class path produces a success
		// result whose `actor` payload carries the requested label and a class
		// path, and the spawned actor is resolvable by label afterwards (so an
		// agent can chain actor_find / future modify tools off it).
		It("spawns an actor from a native class path", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), Registry.TryGet(TEXT("unreal_open_mcp_actor_create"), Handler));

			const FString Label = FString(TestActorPrefix) + TEXT("Native");
			const FString Body = FString::Printf(
				TEXT("{\"classPath\":\"/Script/Engine.StaticMeshActor\",\"name\":\"%s\"}"), *Label);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			const TSharedPtr<FJsonObject>* ActorObj = nullptr;
			TestTrue(TEXT("actor field"), Json->TryGetObjectField(TEXT("actor"), ActorObj));
			if (ActorObj == nullptr || !ActorObj->IsValid())
			{
				return;
			}
			TestEqual(TEXT("label"), (*ActorObj)->GetStringField(TEXT("label")), Label);
			TestTrue(
				TEXT("class is StaticMeshActor"),
				(*ActorObj)->GetStringField(TEXT("class")).Contains(TEXT("StaticMeshActor")));

			// Track for cleanup + verify the actor is real and resolvable.
			AActor* Spawned = FUnrealOpenMcpObjectRef::ResolveActor(Label, Scope.World);
			if (TestNotNull(TEXT("spawned actor resolvable"), Spawned))
			{
				Scope.Track(Spawned);
			}
		});

		// Invalid class path → class_not_found (not a crash, not a silent
		// empty success). The error message echoes the attempted path.
		It("returns class_not_found for an invalid class path", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_create"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"classPath\":\"/Game/Does/Not/Exist.Bogus_C\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("class_not_found")));
		});

		// Missing classPath → missing_parameter (distinct from class_not_found
		// so an agent can tell "I forgot the arg" from "the path was wrong").
		It("returns missing_parameter when classPath is absent", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_create"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});

		// Parent attachment: spawning with a `parent` ref attaches the new
		// actor to the parent (GetAttachParentActor() == parent). The parent
		// is created by setup and tracked; the child is tracked after spawn.
		It("attaches the spawned actor to a parent actor", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			AActor* Parent = Scope.Spawn(FString(TestActorPrefix) + TEXT("Parent"));
			if (!TestNotNull(TEXT("parent spawned"), Parent))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_create"), Handler);

			const FString ChildLabel = FString(TestActorPrefix) + TEXT("Child");
			const FString Body = FString::Printf(
				TEXT("{\"classPath\":\"/Script/Engine.StaticMeshActor\",\"name\":\"%s\",\"parent\":\"%sParent\"}"),
				*ChildLabel, TestActorPrefix);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			AActor* Child = FUnrealOpenMcpObjectRef::ResolveActor(ChildLabel, Scope.World);
			if (TestNotNull(TEXT("child resolvable"), Child))
			{
				Scope.Track(Child);
				TestEqual(TEXT("child attached to parent"), Child->GetAttachParentActor(), Parent);
			}
		});

		// Abstract base class rejection: AActor itself is abstract and must be
		// rejected with invalid_parameter (SpawnActor would otherwise return
		// null and the failure reason would be opaque).
		It("rejects an abstract class with invalid_parameter", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_create"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"classPath\":\"/Script/Engine.Actor\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});

		// Parent miss → parent_not_found, and nothing is spawned (no orphan
		// actor left in the level). The resolve-parent-before-spawn ordering is
		// the load-bearing invariant here.
		It("returns parent_not_found and spawns nothing for a bad parent ref", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_create"), Handler);

			const FString ChildLabel = FString(TestActorPrefix) + TEXT("OrphanCheck");
			const FString Body = FString::Printf(
				TEXT("{\"classPath\":\"/Script/Engine.StaticMeshActor\",\"name\":\"%s\",\"parent\":\"UnrealOpenMcpTestActor_Create_NoSuchParent\"}"),
				*ChildLabel);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("parent_not_found")));

			// Nothing spawned: the label must not resolve.
			AActor* NotSpawned = FUnrealOpenMcpObjectRef::ResolveActor(ChildLabel, Scope.World);
			TestNull(TEXT("no orphan actor left in level"), NotSpawned);
		});

		// Malformed body → invalid_parameter, not a crash.
		It("returns invalid_parameter for a malformed body", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_create"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("<<<not json>>>"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});
	});

	Describe("unreal_open_mcp_actor_create — HTTP round-trip", [this]()
	{
		// Full POST → registry → game-thread handler → {ok,result} envelope on
		// a real loopback socket, and the spawned actor is real (resolvable
		// after the round-trip). Cleanup via FActorScope covers the HTTP path.
		LatentIt(
			"returns the {ok,result} envelope with an actor payload over POST /tools/{name}",
			FTimespan::FromSeconds(30),
			[this](const FDoneDelegate& Done)
			{
				Async(EAsyncExecution::Thread, [this, Done]()
				{
					FActorScope Scope;
					Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld();

					FDispatchHarness Harness;
					if (!TestTrue(TEXT("server started"), Harness.Start(TEXT("/tmp/test"))))
					{
						Done.Execute();
						return;
					}

					const FString Label = FString(TestActorPrefix) + TEXT("HTTP");
					const FString Body = FString::Printf(
						TEXT("{\"classPath\":\"/Script/Engine.StaticMeshActor\",\"name\":\"%s\"}"), *Label);
					const FString Response = SendHttpPost(
						Harness.Server->GetPort(),
						TEXT("/tools/unreal_open_mcp_actor_create"),
						Body);
					Harness.Stop();

					TestTrue(TEXT("HTTP body present"), Response.Contains(TEXT("HTTP/")));
					const FString RespBody = ExtractHttpBody(Response);
					TestTrue(TEXT("ok:true"), RespBody.Contains(TEXT("\"ok\":true")));
					TestTrue(TEXT("actor field"), RespBody.Contains(TEXT("\"actor\"")));

					// Track the HTTP-spawned actor for cleanup; it should be
					// resolvable now that the round-trip completed.
					if (Scope.World != nullptr)
					{
						if (AActor* Spawned = FUnrealOpenMcpObjectRef::ResolveActor(Label, Scope.World))
						{
							Scope.Track(Spawned);
						}
					}
					Done.Execute();
				});
			});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
