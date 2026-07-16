// unreal_open_mcp_actor_set_parent + unreal_open_mcp_actor_duplicate +
// unreal_open_mcp_actor_destroy Automation specs (P2.5).
//
// Pins the three actor tree-ops tools end-to-end at the handler level (no HTTP
// — the dispatch spine is already pinned by earlier specs):
//   1. set_parent: reparents a child under a parent (GetAttachParentActor
//      reflects it); the cycle guard rejects a descendant-as-parent attach with
//      would_create_cycle BEFORE anything mutates; the self-parent case is its
//      own would_create_cycle; a bad child/parent ref returns actor_not_found /
//      parent_not_found with nothing mutated.
//   2. duplicate: clones an actor (the clone is a distinct resolvable actor,
//      same class, label de-duplicated); a `name` arg renames the clone; a bad
//      source ref returns actor_not_found with nothing spawned.
//   3. destroy: single-target destroys the actor (a follow-up find resolves
//      nothing); batch destroys every target in `actors` (count matches); a bad
//      ref in the batch returns actor_not_found with NOTHING destroyed (the
//      resolve-before-transaction ordering — no partial batch).
// Plus one HTTP round-trip for set_parent so the {ok,result} envelope is pinned
// through the real transport.
//
// Cleanup: every spawned actor is destroyed at the end of the case (RAII
// FActorScope with Track + a stray sweep, copied from the P2.4 modify spec).
// Names are prefixed "UnrealOpenMcpTestActor_Tree_" so clean-up finds exactly
// what we spawned.
//
// Adapted from Unity's GameObjectsToolsTests
// (packages/bridge/Tests/Editor/TypedTools/GameObjectsToolsTests.cs —
// set-parent + duplicate + destroy cases) at adapt fidelity: string refs
// replace Unity's instance_id/path/name; AttachToActor + IsAttachedTo replace
// Unity's Undo.SetTransformParent + upward parent-chain walk; EditorDestroyActor
// replaces Unity's Undo.DestroyObjectImmediate; single + batch destroy is folded
// into one tool (Unity's destroy is single-target only).
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

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
#include "Components/SceneComponent.h"   // USceneComponent — root for the actors

#include "Common/TcpSocketBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpActorTreeOpsSpec,
	"UnrealOpenMcp.Tools.ActorTreeOps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpActorTreeOpsSpec)

namespace
{
	/** Prefix for every test actor so clean-up finds exactly what we spawned. */
	constexpr const TCHAR* TestActorPrefix = TEXT("UnrealOpenMcpTestActor_Tree_");

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

		FSocket* Client = FTcpSocketBuilder(TEXT("UnrealOpenMcpActorTreeOpsTestClient"))
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
	 * Spawn an actor with a root SceneComponent so it can be attached/detached.
	 * A bare AActor has no root by default, so set_parent / attach operations
	 * would silently no-op without one. Returns the actor (tracked by @p Scope).
	 */
	AActor* SpawnWithRoot(struct FActorScope& Scope, const FString& Label);

	/**
	 * RAII spawn-tracking helper, copied from the P2.4 modify spec. The scope
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

	AActor* SpawnWithRoot(FActorScope& Scope, const FString& Label)
	{
		AActor* Actor = Scope.Spawn(Label);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		// Give the actor a root scene component so AttachToActor has something
		// to route through. RegisterComponent brings it live.
		USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
		Actor->SetRootComponent(Root);
		Root->RegisterComponent();
		return Actor;
	}

	/** Ephemeral dispatch harness wired with the actor tools registered.
	 *  Mirrors the P2.4 FDispatchHarness; lives on the stack so teardown is
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

void FUnrealOpenMcpActorTreeOpsSpec::Define()
{
	Describe("unreal_open_mcp_actor_set_parent — handler contract", [this]()
	{
		// Reparent: a child attached under a parent shows up via
		// GetAttachParentActor. The core set-parent contract.
		It("reparents a child under a parent", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			AActor* Parent = SpawnWithRoot(Scope, FString(TestActorPrefix) + TEXT("Parent"));
			AActor* Child = SpawnWithRoot(Scope, FString(TestActorPrefix) + TEXT("Child"));
			if (!TestNotNull(TEXT("parent"), Parent) || !TestNotNull(TEXT("child"), Child))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), Registry.TryGet(TEXT("unreal_open_mcp_actor_set_parent"), Handler));

			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"parent\":\"%s\"}"),
				*Child->GetActorLabel(), *Parent->GetActorLabel());
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			// The child's attach parent is now the parent actor.
			TestEqual(TEXT("child attached to parent"), Child->GetAttachParentActor(), Parent);
		});

		// Cycle guard: attaching a parent UNDER one of its own descendants is
		// rejected up front with would_create_cycle. The engine would otherwise
		// silently drop the attach; the explicit rejection lets an agent branch.
		It("rejects a cycle-forming reparent with would_create_cycle", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			// Build A → B (B is the child of A). Attempting to reparent A under B
			// would form a cycle.
			AActor* A = SpawnWithRoot(Scope, FString(TestActorPrefix) + TEXT("CycleA"));
			AActor* B = SpawnWithRoot(Scope, FString(TestActorPrefix) + TEXT("CycleB"));
			if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B))
			{
				return;
			}
			B->AttachToActor(A, FAttachmentTransformRules::KeepWorldTransform);

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_set_parent"), Handler);

			// Reparent A under B — B is a descendant of A, so this is a cycle.
			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"parent\":\"%s\"}"),
				*A->GetActorLabel(), *B->GetActorLabel());
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("would_create_cycle")));

			// The existing hierarchy is untouched (A is still root, B still child of A).
			TestEqual(TEXT("A still root"), A->GetAttachParentActor(), nullptr);
			TestEqual(TEXT("B still child of A"), B->GetAttachParentActor(), A);
		});

		// Self-parent is its own would_create_cycle (distinct from the cycle
		// case so the message is actionable).
		It("rejects self-parent with would_create_cycle", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			AActor* A = SpawnWithRoot(Scope, FString(TestActorPrefix) + TEXT("Self"));
			if (!TestNotNull(TEXT("A"), A))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_set_parent"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"parent\":\"%s\"}"),
				*A->GetActorLabel(), *A->GetActorLabel());
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("would_create_cycle")));
		});

		// Bad child ref → actor_not_found, nothing mutated.
		It("returns actor_not_found for a bad child ref and mutates nothing", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			AActor* Parent = SpawnWithRoot(Scope, FString(TestActorPrefix) + TEXT("ParentMiss"));
			if (!TestNotNull(TEXT("parent"), Parent))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_set_parent"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"actor\":\"UnrealOpenMcpTestActor_Tree_NoSuch\",\"parent\":\"UnrealOpenMcpTestActor_Tree_ParentMiss\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("actor_not_found")));
		});
	});

	Describe("unreal_open_mcp_actor_duplicate — handler contract", [this]()
	{
		// Duplicate: the clone is a distinct, resolvable actor of the same class.
		// The label is de-duplicated so the clone does not collide with the source.
		It("duplicates an actor into a distinct resolvable clone", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString SourceLabel = FString(TestActorPrefix) + TEXT("DupSrc");
			AActor* Source = SpawnWithRoot(Scope, SourceLabel);
			if (!TestNotNull(TEXT("source"), Source))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), Registry.TryGet(TEXT("unreal_open_mcp_actor_duplicate"), Handler));

			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"name\":\"%s_Dup\"}"),
				*SourceLabel, *SourceLabel);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			const TSharedPtr<FJsonObject>* ActorObj = nullptr;
			TestTrue(TEXT("actor payload present"), Json->TryGetObjectField(TEXT("actor"), ActorObj));
			if (ActorObj == nullptr || !ActorObj->IsValid())
			{
				return;
			}
			const FString CloneLabel = (*ActorObj)->GetStringField(TEXT("label"));
			TestTrue(TEXT("clone label set"), CloneLabel.Equals(FString(SourceLabel) + TEXT("_Dup")));

			// The clone resolves as a distinct actor (the scope tracks it via
			// the stray sweep on the test prefix; Track the result's path too).
			AActor* Clone = FUnrealOpenMcpObjectRef::ResolveActor(CloneLabel, Scope.World);
			if (TestNotNull(TEXT("clone resolvable"), Clone))
			{
				Scope.Track(Clone);
				TestNotEqual(TEXT("clone != source"), Clone, Source);
				TestEqual(TEXT("same class"), Clone->GetClass(), Source->GetClass());
			}
		});

		// Bad source ref → actor_not_found, nothing spawned (no orphan).
		It("returns actor_not_found for a bad source ref and spawns nothing", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const int32 BeforeCount = [this]()
			{
				int32 Count = 0;
				UWorld* W = FUnrealOpenMcpObjectRef::GetEditorWorld();
				if (W == nullptr) return -1;
				for (TActorIterator<AActor> It(W); It; ++It)
				{
					if ((*It) != nullptr && (*It)->GetActorLabel().StartsWith(TestActorPrefix, ESearchCase::CaseSensitive))
					{
						++Count;
					}
				}
				return Count;
			}();

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_duplicate"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"actor\":\"UnrealOpenMcpTestActor_Tree_NoSuchSource\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("actor_not_found")));

			// No orphan actor leaked — the test-prefixed actor count is unchanged.
			if (BeforeCount >= 0)
			{
				int32 AfterCount = 0;
				for (TActorIterator<AActor> It(Scope.World); It; ++It)
				{
					if ((*It) != nullptr && (*It)->GetActorLabel().StartsWith(TestActorPrefix, ESearchCase::CaseSensitive))
					{
						++AfterCount;
					}
				}
				TestEqual(TEXT("no orphan spawned"), AfterCount, BeforeCount);
			}
		});
	});

	Describe("unreal_open_mcp_actor_destroy — handler contract", [this]()
	{
		// Single destroy: the actor is gone after the call (a follow-up resolve
		// finds nothing).
		It("destroys a single actor", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("DestroySingle");
			AActor* Actor = SpawnWithRoot(Scope, Label);
			if (!TestNotNull(TEXT("spawned"), Actor))
			{
				return;
			}
			// Spawned-but-destroyed actors should NOT be re-destroyed by the
			// scope's destructor — remove it from the tracked list so the scope
			// doesn't try to destroy it twice (DestroyActor is a no-op on a
			// pending-kill actor, but the double-call is noisy).
			Scope.Spawned.Remove(Actor);

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), Registry.TryGet(TEXT("unreal_open_mcp_actor_destroy"), Handler));

			const FString Body = FString::Printf(TEXT("{\"actor\":\"%s\"}"), *Label);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("count == 1"), Json->GetNumberField(TEXT("count")), 1.0);

			// The actor no longer resolves.
			TestNull(TEXT("actor gone"), FUnrealOpenMcpObjectRef::ResolveActor(Label, Scope.World));
		});

		// Batch destroy: every target in `actors` is destroyed; count matches.
		It("destroys a batch of actors", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString LabelA = FString(TestActorPrefix) + TEXT("DestroyBatchA");
			const FString LabelB = FString(TestActorPrefix) + TEXT("DestroyBatchB");
			const FString LabelC = FString(TestActorPrefix) + TEXT("DestroyBatchC");
			AActor* A = SpawnWithRoot(Scope, LabelA);
			AActor* B = SpawnWithRoot(Scope, LabelB);
			AActor* C = SpawnWithRoot(Scope, LabelC);
			if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B) || !TestNotNull(TEXT("C"), C))
			{
				return;
			}
			Scope.Spawned.Remove(A);
			Scope.Spawned.Remove(B);
			Scope.Spawned.Remove(C);

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_destroy"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"actors\":[\"%s\",\"%s\",\"%s\"]}"),
				*LabelA, *LabelB, *LabelC);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("count == 3"), Json->GetNumberField(TEXT("count")), 3.0);

			// All three are gone.
			TestNull(TEXT("A gone"), FUnrealOpenMcpObjectRef::ResolveActor(LabelA, Scope.World));
			TestNull(TEXT("B gone"), FUnrealOpenMcpObjectRef::ResolveActor(LabelB, Scope.World));
			TestNull(TEXT("C gone"), FUnrealOpenMcpObjectRef::ResolveActor(LabelC, Scope.World));
		});

		// Batch with a bad ref: resolve-before-transaction means NOTHING is
		// destroyed (no partial batch). The load-bearing invariant.
		It("returns actor_not_found on a bad ref in a batch and destroys nothing", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString LabelA = FString(TestActorPrefix) + TEXT("BatchKeepA");
			const FString LabelB = FString(TestActorPrefix) + TEXT("BatchKeepB");
			AActor* A = SpawnWithRoot(Scope, LabelA);
			AActor* B = SpawnWithRoot(Scope, LabelB);
			if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_destroy"), Handler);

			// Batch with one bad ref in the middle.
			const FString Body = FString::Printf(
				TEXT("{\"actors\":[\"%s\",\"UnrealOpenMcpTestActor_Tree_NoSuch\",\"%s\"]}"),
				*LabelA, *LabelB);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("actor_not_found")));

			// The real actors are untouched (resolve-before-transaction).
			TestNotNull(TEXT("A still alive"), FUnrealOpenMcpObjectRef::ResolveActor(LabelA, Scope.World));
			TestNotNull(TEXT("B still alive"), FUnrealOpenMcpObjectRef::ResolveActor(LabelB, Scope.World));
		});

		// Missing both actor and actors → missing_parameter.
		It("returns missing_parameter when neither actor nor actors is supplied", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_destroy"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});
	});

	Describe("unreal_open_mcp_actor_set_parent — HTTP round-trip", [this]()
	{
		// Full POST → registry → game-thread handler → {ok,result} envelope on
		// a real loopback socket, and the attach is visible after the round-trip.
		LatentIt(
			"returns the {ok,result} envelope with the actor payload over POST /tools/{name}",
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
					AActor* Parent = SpawnWithRoot(Scope, FString(TestActorPrefix) + TEXT("HTTPParent"));
					AActor* Child = SpawnWithRoot(Scope, FString(TestActorPrefix) + TEXT("HTTPChild"));
					if (Parent == nullptr || Child == nullptr)
					{
						TestNotNull(TEXT("parent"), Parent);
						TestNotNull(TEXT("child"), Child);
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
						TEXT("{\"actor\":\"%s\",\"parent\":\"%s\"}"),
						*Child->GetActorLabel(), *Parent->GetActorLabel());
					const FString Response = SendHttpPost(
						Harness.Server->GetPort(),
						TEXT("/tools/unreal_open_mcp_actor_set_parent"),
						Body);
					Harness.Stop();

					TestTrue(TEXT("HTTP body present"), Response.Contains(TEXT("HTTP/")));
					const FString RespBody = ExtractHttpBody(Response);
					TestTrue(TEXT("ok:true"), RespBody.Contains(TEXT("\"ok\":true")));
					TestTrue(TEXT("actor payload"), RespBody.Contains(TEXT("\"actor\"")));
					// The attach is visible after the round-trip.
					TestEqual(TEXT("child attached after round-trip"), Child->GetAttachParentActor(), Parent);
					Done.Execute();
				});
			});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
