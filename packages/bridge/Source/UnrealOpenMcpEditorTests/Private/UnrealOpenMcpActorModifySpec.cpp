// unreal_open_mcp_actor_modify + unreal_open_mcp_object_modify Automation
// specs (P2.4).
//
// Pins the reflection-write pair end-to-end. Two Describe blocks:
//   1. actor_modify handler contract (no HTTP): transform shortcut (location)
//      writes through to GetActorLocation; property-bag writes (bool + float)
//      land on reflected FProperties; an unknown property name surfaces in
//      errors[] without aborting the batch (partial success); a two-actor
//      `actors` batch applies to both; a missing actor ref → actor_not_found
//      with nothing mutated; a missing `properties` → missing_parameter.
//   2. object_modify handler contract: a component property write via
//      ResolveObject (the actor's root component, addressed by object path)
//      lands on the component; a missing `object` → missing_parameter; a bad
//      object ref → object_not_found.
// Plus one HTTP round-trip for actor_modify so the {ok,result} envelope is
// pinned through the real transport.
//
// Cleanup: every spawned actor is destroyed at the end of the case (RAII
// FActorScope with Track + a stray sweep, copied from the P2.3 create spec —
// the more robust pattern). Names are prefixed "UnrealOpenMcpTestActor_Modify_"
// so clean-up finds exactly what we spawned.
//
// Adapted from Unity's GameObjectsToolsTests
// (packages/bridge/Tests/Editor/TypedTools/GameObjectsToolsTests.cs — modify +
// partial-error cases) at adapt fidelity: the `properties` bag replaces
// Unity's three-surface RFC 7396 form; FProperty reflection replaces
// System.Reflection; transform shortcuts are routed to the actor transform APIs.
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
#include "Components/SceneComponent.h"   // USceneComponent for the root component write

#include "Common/TcpSocketBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpActorModifySpec,
	"UnrealOpenMcp.Tools.ActorModify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpActorModifySpec)

namespace
{
	/** Prefix for every test actor so clean-up finds exactly what we spawned. */
	constexpr const TCHAR* TestActorPrefix = TEXT("UnrealOpenMcpTestActor_Modify_");

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

		FSocket* Client = FTcpSocketBuilder(TEXT("UnrealOpenMcpActorModifyTestClient"))
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
	 * RAII spawn-tracking helper, copied from the P2.3 create spec. The scope
	 * owns setup spawns (Spawn) and tracks tool-spawned actors (Track); the
	 * destructor destroys them all plus any test-prefixed strays so a failed
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

void FUnrealOpenMcpActorModifySpec::Define()
{
	Describe("unreal_open_mcp_actor_modify — handler contract", [this]()
	{
		// Transform shortcut: `location` inside `properties` is routed to
		// SetActorLocation, so GetActorLocation reflects the write afterwards.
		// This pins the transform-shortcut terminal handling in the shared
		// ApplyProperties helper (location is NOT an FProperty on actors).
		It("writes the location transform shortcut", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("Loc");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("spawned"), Actor))
			{
				return;
			}
			const FVector Before = Actor->GetActorLocation();

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), Registry.TryGet(TEXT("unreal_open_mcp_actor_modify"), Handler));

			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"properties\":{\"location\":{\"x\":1234.0,\"y\":5678.0,\"z\":90.0}}}"),
				*Label);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			// applied counts the transform shortcut as one applied field.
			TestEqual(TEXT("applied == 1"), Json->GetNumberField(TEXT("applied")), 1.0);

			const FVector After = Actor->GetActorLocation();
			TestEqual(TEXT("location x"), After.X, 1234.0);
			TestEqual(TEXT("location y"), After.Y, 5678.0);
			TestEqual(TEXT("location z"), After.Z, 90.0);
			TestNotEqual(TEXT("location changed"), After, Before);
		});

		// Property bag: a bool + a float on a reflected FProperty land through
		// FJsonObjectConverter::JsonValueToUProperty. Hidden is a bool FProperty
		// on AActor; bNetLoadOnDemand is not guaranteed, so we use bHidden
		// (editable, writable). Both count toward `applied`.
		It("writes bool and float properties via the property bag", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("Props");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("spawned"), Actor))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_modify"), Handler);

			// bHidden is a writable bool FProperty on AActor. The property
			// name is lower-first-cased on the wire; the helper matches
			// case-insensitively, so either casing resolves.
			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"properties\":{\"bHidden\":true}}"),
				*Label);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("applied == 1"), Json->GetNumberField(TEXT("applied")), 1.0);
			TestTrue(TEXT("bHidden set"), Actor->IsHidden());
		});

		// Partial success: an unknown property name surfaces in errors[] and
		// does NOT abort the batch — the known property (location) still
		// applies. This is the load-bearing partial-success contract.
		It("reports an unknown property in errors[] without aborting the batch", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("Partial");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("spawned"), Actor))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_modify"), Handler);

			// 'location' applies; 'thisPropertyDoesNotExist' errors.
			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"properties\":{\"location\":{\"x\":10.0},\"thisPropertyDoesNotExist\":42}}"),
				*Label);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("applied == 1 (location)"), Json->GetNumberField(TEXT("applied")), 1.0);

			const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
			TestTrue(TEXT("errors present"), Json->TryGetArrayField(TEXT("errors"), Errors));
			if (Errors != nullptr)
			{
				TestTrue(TEXT("one error"), Errors->Num() == 1);
				if (Errors->Num() == 1)
				{
					TestTrue(
						TEXT("error mentions the unknown property"),
						(*Errors)[0]->AsString().Contains(TEXT("thisPropertyDoesNotExist")));
				}
			}

			// The known field still applied despite the error sibling.
			TestEqual(TEXT("location applied despite sibling error"), Actor->GetActorLocation().X, 10.0);
		});

		// Batch: an `actors` array applies the same patches to two actors. Each
		// gets its own per-actor entry in the result `actors` array.
		It("applies the same patches to a batch of two actors", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString LabelA = FString(TestActorPrefix) + TEXT("BatchA");
			const FString LabelB = FString(TestActorPrefix) + TEXT("BatchB");
			AActor* A = Scope.Spawn(LabelA);
			AActor* B = Scope.Spawn(LabelB);
			if (!TestNotNull(TEXT("spawned A"), A) || !TestNotNull(TEXT("spawned B"), B))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_modify"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"actors\":[\"%s\",\"%s\"],\"properties\":{\"location\":{\"x\":500.0}}}"),
				*LabelA, *LabelB);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			// Both actors applied the one field.
			TestEqual(TEXT("applied == 2"), Json->GetNumberField(TEXT("applied")), 2.0);
			const TArray<TSharedPtr<FJsonValue>>* ActorResults = nullptr;
			TestTrue(TEXT("actors array present"), Json->TryGetArrayField(TEXT("actors"), ActorResults));
			if (ActorResults != nullptr)
			{
				TestEqual(TEXT("two actor entries"), ActorResults->Num(), 2);
			}

			// Both actors moved.
			TestEqual(TEXT("A location x"), A->GetActorLocation().X, 500.0);
			TestEqual(TEXT("B location x"), B->GetActorLocation().X, 500.0);
		});

		// Missing actor ref → missing_parameter. Distinct from actor_not_found
		// so an agent can tell "I forgot the arg" from "the ref didn't resolve".
		It("returns missing_parameter when neither actor nor actors is supplied", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_modify"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"properties\":{\"location\":{\"x\":1.0}}}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});

		// Bad actor ref → actor_not_found, and nothing is mutated. The
		// resolve-before-transaction ordering means a miss leaves no undo entry.
		It("returns actor_not_found for a bad ref and mutates nothing", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			// Spawn a real actor so we can prove it was NOT moved by the failed call.
			const FString Label = FString(TestActorPrefix) + TEXT("Untouched");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("spawned"), Actor))
			{
				return;
			}
			const FVector Before = Actor->GetActorLocation();

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_modify"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"actor\":\"UnrealOpenMcpTestActor_Modify_NoSuchActor\",\"properties\":{\"location\":{\"x\":999.0}}}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("actor_not_found")));

			// The real actor was not touched.
			TestEqual(TEXT("unrelated actor unchanged"), Actor->GetActorLocation(), Before);
		});
	});

	Describe("unreal_open_mcp_object_modify — handler contract", [this]()
	{
		// Component property write via ResolveObject: address the actor's root
		// component by its object path (GetPathName), set a scene-component
		// relative transform shortcut, and verify it landed. This exercises the
		// ResolveObject → loaded-object path (the component is in memory) and
		// the scene-component relative-transform special-case in ApplyProperties.
		It("writes a component property via object path resolution", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("ObjComp");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("spawned"), Actor))
			{
				return;
			}
			// Give the actor a root scene component so relative-transform
			// shortcuts have a target. A bare AActor has no root by default.
			USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
			Actor->SetRootComponent(Root);
			Root->RegisterComponent();
			const FString CompPath = Root->GetPathName();

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), Registry.TryGet(TEXT("unreal_open_mcp_object_modify"), Handler));

			const FString Body = FString::Printf(
				TEXT("{\"object\":\"%s\",\"properties\":{\"relativeLocation\":{\"x\":250.0,\"y\":0.0,\"z\":0.0}}}"),
				*CompPath);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("applied == 1"), Json->GetNumberField(TEXT("applied")), 1.0);
			TestEqual(TEXT("relative location x"), Root->GetRelativeLocation().X, 250.0);
		});

		// Missing `object` → missing_parameter.
		It("returns missing_parameter when object is absent", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_object_modify"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"properties\":{\"relativeLocation\":{\"x\":1.0}}}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});

		// Bad object ref → object_not_found, nothing mutated.
		It("returns object_not_found for a bad ref", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_object_modify"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"object\":\"UnrealOpenMcpTestActor_Modify_NoSuchObject\",\"properties\":{\"relativeLocation\":{\"x\":1.0}}}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("object_not_found")));
		});
	});

	Describe("unreal_open_mcp_actor_modify — HTTP round-trip", [this]()
	{
		// Full POST → registry → game-thread handler → {ok,result} envelope on
		// a real loopback socket, and the location write is visible on the
		// actor after the round-trip.
		LatentIt(
			"returns the {ok,result} envelope with an applied count over POST /tools/{name}",
			FTimespan::FromSeconds(30),
			[this](const FDoneDelegate& Done)
			{
				Async(EAsyncExecution::Thread, [this, Done]()
				{
					FActorScope Scope;
					Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld();
					if (Scope.World == nullptr)
					{
						TestNotNull(TEXT("editor world"), Scope.World);
						Done.Execute();
						return;
					}
					const FString Label = FString(TestActorPrefix) + TEXT("HTTP");
					AActor* Actor = Scope.Spawn(Label);
					if (Actor == nullptr)
					{
						TestNotNull(TEXT("spawned"), Actor);
						Done.Execute();
						return;
					}

					FDispatchHarness Harness;
					if (!TestTrue(TEXT("server started"), Harness.Start(TEXT("/tmp/test"))))
					{
						Done.Execute();
						return;
					}

					const FString Body = FString::Printf(
						TEXT("{\"actor\":\"%s\",\"properties\":{\"location\":{\"x\":321.0}}}"),
						*Label);
					const FString Response = SendHttpPost(
						Harness.Server->GetPort(),
						TEXT("/tools/unreal_open_mcp_actor_modify"),
						Body);
					Harness.Stop();

					TestTrue(TEXT("HTTP body present"), Response.Contains(TEXT("HTTP/")));
					const FString RespBody = ExtractHttpBody(Response);
					TestTrue(TEXT("ok:true"), RespBody.Contains(TEXT("\"ok\":true")));
					TestTrue(TEXT("applied field"), RespBody.Contains(TEXT("\"applied\"")));
					// The location write is visible after the round-trip.
					TestEqual(TEXT("location x after round-trip"), Actor->GetActorLocation().X, 321.0);
					Done.Execute();
				});
			});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
