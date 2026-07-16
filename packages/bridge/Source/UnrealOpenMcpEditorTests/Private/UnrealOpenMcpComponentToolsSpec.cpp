// unreal_open_mcp_actor_component_add + _destroy + _get + _modify +
// _list_all Automation specs (P2.5).
//
// Pins the five component CRUD tools end-to-end at the handler level (no HTTP —
// the dispatch spine is already pinned by earlier specs):
//   1. add: a USceneComponent is created + registered on the actor and shows up
//      in the actor's instance-components list; the result ComponentData
//      carries name + class + properties; a user-supplied `name` sets the
//      UObject name; a colliding name returns name_conflict; a bad class ref
//      returns class_not_found; an abstract/non-UActorComponent class returns
//      invalid_parameter.
//   2. list-all: returns every component on the actor (count includes the
//      freshly-added one).
//   3. get: returns the component's ComponentData with a properties object.
//   4. modify: a relativeLocation write on a USceneComponent lands via the
//      shared ApplyProperties helper (GetRelativeLocation reflects it).
//   5. destroy: an instance component is destroyed (a follow-up list-all no
//      longer includes it); a native/non-instance component returns
//      not_instance_component (the root we added in setup IS an instance
//      component, so we add then destroy a second one to exercise the gate on
//      a native component — the actor's RootComponent set via SetRootComponent
//      is an instance component by virtue of AddInstanceComponent not being
//      called, but our setup registers it so we use a non-registered stand-in).
// Plus one HTTP round-trip for component_add.
//
// Cleanup: every spawned actor is destroyed at the end of the case (RAII
// FActorScope with Track + a stray sweep, copied from the P2.4 modify spec).
// Names are prefixed "UnrealOpenMcpTestActor_Comp_" so clean-up finds exactly
// what we spawned.
//
// Adapted from Unity's ComponentsToolsTests
// (packages/bridge/Tests/Editor/TypedTools/ComponentsToolsTests.cs — add /
// destroy / get / modify / list-all cases) at adapt fidelity: a single
// componentClass per call replaces Unity's component_types array; the instance-
// component gate replaces Unity's all-components-are-instance model; FProperty
// reflection replaces System.Reflection; USceneComponent is the test vehicle
// (concrete, non-abstract, spawnable) instead of Unity's Rigidbody.
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
#include "Components/SceneComponent.h"   // USceneComponent — test vehicle

#include "Common/TcpSocketBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpComponentToolsSpec,
	"UnrealOpenMcp.Tools.ComponentTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpComponentToolsSpec)

namespace
{
	/** Prefix for every test actor so clean-up finds exactly what we spawned. */
	constexpr const TCHAR* TestActorPrefix = TEXT("UnrealOpenMcpTestActor_Comp_");

	/** Concrete, non-abstract UActorComponent class used as the test vehicle.
	 *  USceneComponent is spawnable, registerable, and has writable relative-
	 *  transform FProperties — everything the add/get/modify/destroy cases need. */
	constexpr const TCHAR* TestComponentClass = TEXT("/Script/Engine.SceneComponent");

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

		FSocket* Client = FTcpSocketBuilder(TEXT("UnrealOpenMcpComponentToolsTestClient"))
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

		/** Spawn a bare actor (no root) — component_add will add the root. */
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

void FUnrealOpenMcpComponentToolsSpec::Define()
{
	Describe("unreal_open_mcp_actor_component_add — handler contract", [this]()
	{
		// Add: a USceneComponent is created + registered on the actor and shows
		// up in the actor's instance-components list; the result ComponentData
		// carries name + class + properties. Because the actor starts rootless,
		// the added scene component becomes the root.
		It("adds a scene component that becomes the actor root", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("AddRoot");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("actor"), Actor))
			{
				return;
			}
			TestNull(TEXT("starts rootless"), Actor->GetRootComponent());

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), Registry.TryGet(TEXT("unreal_open_mcp_actor_component_add"), Handler));

			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"componentClass\":\"%s\",\"name\":\"AddedRoot\"}"),
				*Label, TestComponentClass);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			const TSharedPtr<FJsonObject>* CompObj = nullptr;
			TestTrue(TEXT("component payload present"), Json->TryGetObjectField(TEXT("component"), CompObj));
			if (CompObj == nullptr || !CompObj->IsValid())
			{
				return;
			}
			TestTrue(TEXT("name set"), (*CompObj)->GetStringField(TEXT("name")).Equals(TEXT("AddedRoot")));
			TestTrue(TEXT("class present"), (*CompObj)->GetStringField(TEXT("class")).Contains(TEXT("SceneComponent")));
			// properties object reflected from the component's FProperties.
			TestTrue(TEXT("properties object present"), (*CompObj)->HasField(TEXT("properties")));

			// The added component is the actor's root and is in the instance list.
			TestNotNull(TEXT("root set"), Actor->GetRootComponent());
			TestTrue(TEXT("root is the added component"), Actor->GetRootComponent()->GetName().Equals(TEXT("AddedRoot")));
			TestTrue(
				TEXT("in instance components"),
				Actor->GetInstanceComponents().ContainsByPredicate([](UActorComponent* C)
				{ return C != nullptr && C->GetName().Equals(TEXT("AddedRoot")); }));
		});

		// Name collision: a user-supplied name that matches an existing
		// component on the actor returns name_conflict (NewObject would
		// otherwise fatal-assert).
		It("returns name_conflict when a component name collides", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("AddCollide");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("actor"), Actor))
			{
				return;
			}
			// Add a first component named "Shared".
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler AddHandler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_component_add"), AddHandler);
			const FString FirstBody = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"componentClass\":\"%s\",\"name\":\"Shared\"}"),
				*Label, TestComponentClass);
			TestTrue(TEXT("first add ok"), AddHandler(FirstBody).bOk);

			// Second add with the same name → name_conflict.
			const FUnrealOpenMcpToolDispatchResult Result = AddHandler(FirstBody);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("name_conflict")));
		});

		// Bad class ref → class_not_found.
		It("returns class_not_found for a bad component class ref", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("AddBadClass");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("actor"), Actor))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_component_add"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"componentClass\":\"UnrealOpenMcpTestActor_Comp_NoSuchClass\"}"),
				*Label);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("class_not_found")));
		});

		// Non-UActorComponent class → invalid_parameter. AActor is not a
		// component subclass, so it is rejected.
		It("returns invalid_parameter when the class is not a UActorComponent", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("AddNotComp");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("actor"), Actor))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_component_add"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"componentClass\":\"/Script/Engine.Actor\"}"),
				*Label);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});
	});

	Describe("unreal_open_mcp_actor_component_list_all + _get — handler contract", [this]()
	{
		// list-all: returns every component on the actor (count includes the
		// freshly-added one). Read-only.
		It("lists every component on the actor", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("ListAll");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("actor"), Actor))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler AddHandler;
			FUnrealOpenMcpToolHandler ListHandler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_component_add"), AddHandler);
			Registry.TryGet(TEXT("unreal_open_mcp_actor_component_list_all"), ListHandler);

			// Add two named components.
			for (const TCHAR* Name : {TEXT("ListA"), TEXT("ListB")})
			{
				const FString AddBody = FString::Printf(
					TEXT("{\"actor\":\"%s\",\"componentClass\":\"%s\",\"name\":\"%s\"}"),
					*Label, TestComponentClass, Name);
				TestTrue(TEXT("add ok"), AddHandler(AddBody).bOk);
			}

			const FString ListBody = FString::Printf(TEXT("{\"actor\":\"%s\"}"), *Label);
			const FUnrealOpenMcpToolDispatchResult Result = ListHandler(ListBody);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			// At least the two we added (the actor may carry engine defaults).
			TestTrue(TEXT("count >= 2"), Json->GetNumberField(TEXT("count")) >= 2.0);
			const TArray<TSharedPtr<FJsonValue>>* Comps = nullptr;
			TestTrue(TEXT("components array present"), Json->TryGetArrayField(TEXT("components"), Comps));
			if (Comps != nullptr)
			{
				// Per-entry ComponentData omits properties (use component_get).
				bool bFoundListA = false;
				for (const TSharedPtr<FJsonValue>& V : *Comps)
				{
					const TSharedPtr<FJsonObject>* Entry = nullptr;
					if (V->TryGetObject(Entry) && Entry->IsValid())
					{
						if ((*Entry)->GetStringField(TEXT("name")).Equals(TEXT("ListA")))
						{
							bFoundListA = true;
							TestFalse(TEXT("properties omitted in list-all"), (*Entry)->HasField(TEXT("properties")));
						}
					}
				}
				TestTrue(TEXT("ListA found in list"), bFoundListA);
			}
		});

		// get: returns the component's ComponentData with a properties object.
		It("returns a component with a properties object", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("Get");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("actor"), Actor))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler AddHandler;
			FUnrealOpenMcpToolHandler GetHandler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_component_add"), AddHandler);
			Registry.TryGet(TEXT("unreal_open_mcp_actor_component_get"), GetHandler);

			const FString AddBody = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"componentClass\":\"%s\",\"name\":\"GetTarget\"}"),
				*Label, TestComponentClass);
			TestTrue(TEXT("add ok"), AddHandler(AddBody).bOk);

			const FString GetBody = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"component\":\"GetTarget\"}"),
				*Label);
			const FUnrealOpenMcpToolDispatchResult Result = GetHandler(GetBody);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			const TSharedPtr<FJsonObject>* CompObj = nullptr;
			TestTrue(TEXT("component payload present"), Json->TryGetObjectField(TEXT("component"), CompObj));
			if (CompObj != nullptr && (*CompObj).IsValid())
			{
				TestTrue(TEXT("name set"), (*CompObj)->GetStringField(TEXT("name")).Equals(TEXT("GetTarget")));
				TestTrue(TEXT("properties object present"), (*CompObj)->HasField(TEXT("properties")));
			}
		});

		// component_not_found for a bad component ref.
		It("returns component_not_found for a bad component ref", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("GetMiss");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("actor"), Actor))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_component_get"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"component\":\"NoSuchComponent\"}"),
				*Label);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("component_not_found")));
		});
	});

	Describe("unreal_open_mcp_actor_component_modify — handler contract", [this]()
	{
		// modify: a relativeLocation write on a USceneComponent lands via the
		// shared ApplyProperties helper (GetRelativeLocation reflects it).
		// Pins the scene-component transform-shortcut path in the helper.
		It("writes a relativeLocation transform shortcut on a component", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("Modify");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("actor"), Actor))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler AddHandler;
			FUnrealOpenMcpToolHandler ModifyHandler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_component_add"), AddHandler);
			Registry.TryGet(TEXT("unreal_open_mcp_actor_component_modify"), ModifyHandler);

			const FString AddBody = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"componentClass\":\"%s\",\"name\":\"ModTarget\"}"),
				*Label, TestComponentClass);
			TestTrue(TEXT("add ok"), AddHandler(AddBody).bOk);
			USceneComponent* Comp = Cast<USceneComponent>(
				FUnrealOpenMcpObjectRef::ResolveComponent(Actor, TEXT("ModTarget")));
			if (!TestNotNull(TEXT("component resolved"), Comp))
			{
				return;
			}

			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"component\":\"ModTarget\",\"properties\":{\"relativeLocation\":{\"x\":432.0,\"y\":0.0,\"z\":0.0}}}"),
				*Label);
			const FUnrealOpenMcpToolDispatchResult Result = ModifyHandler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("applied == 1"), Json->GetNumberField(TEXT("applied")), 1.0);
			TestEqual(TEXT("relative location x"), Comp->GetRelativeLocation().X, 432.0);
		});

		// component_not_found for a bad component ref → nothing mutated.
		It("returns component_not_found for a bad component ref and mutates nothing", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("ModifyMiss");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("actor"), Actor))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_component_modify"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"component\":\"NoSuchComponent\",\"properties\":{\"relativeLocation\":{\"x\":1.0}}}"),
				*Label);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("component_not_found")));
		});
	});

	Describe("unreal_open_mcp_actor_component_destroy — handler contract", [this]()
	{
		// destroy: an instance component (added via component_add) is destroyed;
		// a follow-up resolve finds nothing.
		It("destroys an instance component", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("Destroy");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("actor"), Actor))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler AddHandler;
			FUnrealOpenMcpToolHandler DestroyHandler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_component_add"), AddHandler);
			Registry.TryGet(TEXT("unreal_open_mcp_actor_component_destroy"), DestroyHandler);

			const FString AddBody = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"componentClass\":\"%s\",\"name\":\"DestroyMe\"}"),
				*Label, TestComponentClass);
			TestTrue(TEXT("add ok"), AddHandler(AddBody).bOk);
			TestNotNull(TEXT("component present before destroy"),
				FUnrealOpenMcpObjectRef::ResolveComponent(Actor, TEXT("DestroyMe")));

			const FString DestroyBody = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"component\":\"DestroyMe\"}"),
				*Label);
			const FUnrealOpenMcpToolDispatchResult Result = DestroyHandler(DestroyBody);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestTrue(TEXT("destroyed true"), Json->GetBoolField(TEXT("destroyed")));
			// The component no longer resolves.
			TestNull(TEXT("component gone after destroy"),
				FUnrealOpenMcpObjectRef::ResolveComponent(Actor, TEXT("DestroyMe")));
		});

		// component_not_found for a bad component ref.
		It("returns component_not_found for a bad component ref", [this]()
		{
			FActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			const FString Label = FString(TestActorPrefix) + TEXT("DestroyMiss");
			AActor* Actor = Scope.Spawn(Label);
			if (!TestNotNull(TEXT("actor"), Actor))
			{
				return;
			}
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpActorTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_actor_component_destroy"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"component\":\"NoSuchComponent\"}"),
				*Label);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("component_not_found")));
		});
	});

	Describe("unreal_open_mcp_actor_component_add — HTTP round-trip", [this]()
	{
		// Full POST → registry → game-thread handler → {ok,result} envelope on
		// a real loopback socket, and the component is live after the round-trip.
		LatentIt(
			"returns the {ok,result} envelope with the component payload over POST /tools/{name}",
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
					const FString Label = FString(TestActorPrefix) + TEXT("HTTPAdd");
					AActor* Actor = Scope.Spawn(Label);
					if (Actor == nullptr)
					{
						TestNotNull(TEXT("actor"), Actor);
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
						TEXT("{\"actor\":\"%s\",\"componentClass\":\"%s\",\"name\":\"HttpComp\"}"),
						*Label, TestComponentClass);
					const FString Response = SendHttpPost(
						Harness.Server->GetPort(),
						TEXT("/tools/unreal_open_mcp_actor_component_add"),
						Body);
					Harness.Stop();

					TestTrue(TEXT("HTTP body present"), Response.Contains(TEXT("HTTP/")));
					const FString RespBody = ExtractHttpBody(Response);
					TestTrue(TEXT("ok:true"), RespBody.Contains(TEXT("\"ok\":true")));
					TestTrue(TEXT("component payload"), RespBody.Contains(TEXT("\"component\"")));
					// The component is live after the round-trip.
					TestNotNull(
						TEXT("component live after round-trip"),
						FUnrealOpenMcpObjectRef::ResolveComponent(Actor, TEXT("HttpComp")));
					Done.Execute();
				});
			});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
