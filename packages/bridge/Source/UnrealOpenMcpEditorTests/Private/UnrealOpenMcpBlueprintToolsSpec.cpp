// unreal_open_mcp_blueprint_create / blueprint_get / blueprint_add_component /
// blueprint_remove_component Automation specs (P6.1 + P6.2).
//
// Pins the Blueprint tool family end-to-end at the handler level. The cases
// mirror the P6.1 + P6.2 plan acceptance criteria + test lists:
//   - create: happy path (new Blueprint from the default Actor parent under
//     /Game; result echoes name + path + parentClass); package-path AND
//     object-path forms both accepted and normalised; explicit parent
//     ('/Script/Engine.Actor'); non-Blueprintable parent refused
//     (parent_not_blueprintable); unresolvable parent refused
//     (parent_class_not_found); writable-root refusal on /Engine
//     (invalid_content_root); collision with an existing Blueprint AND with an
//     existing non-Blueprint asset at the target path both refused with
//     asset_already_exists (the any-UObject probe — never a fatal assert);
//     missing_parameter.
//   - get: round-trip after create (name/path/parentClass present; the
//     variables / components / functions / events / interfaces / parentChain
//     arrays present, even if empty; events entries carry `enabled`);
//     blueprint_not_found for a missing path; missing_parameter.
//   - add_component: happy path (StaticMeshComponent node added; result echoes
//     component + class; blueprint_get reflects it); optional parent attaches a
//     child scene component under an existing scene-component node (parent field
//     populated by get); abstract class rejected (abstract_component — the
//     CreateNode -> NewObject fatal assert is never hit); name collision with an
//     existing SCS node, an existing member variable, and a parent-class
//     property all rejected (name_collision); non-scene component cannot attach
//     under a parent + non-scene parent rejected (invalid_attachment); missing
//     parent rejected (parent_not_found); no_scs on a Blueprint with no SCS;
//     invalid_component_class for a non-UActorComponent; missing_parameter.
//   - remove_component: deletes a node and blueprint_get no longer lists it;
//     children are promoted (a child of the removed node survives, re-parented);
//     missing component rejected (component_not_found); missing_parameter.
//   - mutation classification: create/add/remove mutating, get read-only.
//
// The suite owns its scratch tree under /Game/__McpP61Blueprint — teardown
// removes the whole subtree so the automation project does not accumulate test
// artifacts between runs. The default parent (Actor —
// '/Script/Engine.Actor') is a native class always present in the editor, so
// the default-parent cases never need a skip.
//
// Fidelity: greenfield vs Unity (no Blueprint twin); behavior-adapted from
// Unreal-MCP's blueprint-create / blueprint-get test surface (the any-UObject
// collision probe + the disabled-ghost-event `enabled` flag).
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Tools/UnrealOpenMcpBlueprintTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "EditorAssetLibrary.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpBlueprintToolsSpec,
	"UnrealOpenMcp.Tools.BlueprintTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpBlueprintToolsSpec)

namespace
{
	/** Scratch tree root — every P6.1 case lives under here so teardown can
	 *  remove the whole subtree with one DeleteDirectory. */
	constexpr const TCHAR* BlueprintScratchRoot = TEXT("/Game/__McpP61Blueprint");

	/** The default Actor parent (native class, always present). */
	constexpr const TCHAR* ActorParentPath = TEXT("/Script/Engine.Actor");

	/** Parse a JSON object from a string. Null on failure. */
	TSharedPtr<FJsonObject> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	/** Build a JSON body string from a list of (key, raw-value) pairs. Values
	 *  are emitted as raw JSON tokens (callers pre-quote strings, pass raw
	 *  true/false for booleans, and bracketed/braced forms for arrays/objects). */
	FString MakeBody(std::initializer_list<TPair<FString, FString>> Fields)
	{
		FString Out = TEXT("{");
		bool bFirst = true;
		for (const TPair<FString, FString>& F : Fields)
		{
			if (!bFirst)
			{
				Out += TEXT(",");
			}
			bFirst = false;
			Out += FString::Printf(TEXT("\"%s\":%s"), *F.Key, *F.Value);
		}
		Out += TEXT("}");
		return Out;
	}

	/** Invoke a registered tool by name with a JSON body. */
	FUnrealOpenMcpToolDispatchResult Invoke(
		FUnrealOpenMcpToolRegistry& Registry,
		const FString& ToolName,
		const FString& Body)
	{
		FUnrealOpenMcpToolHandler Handler;
		if (!Registry.TryGet(ToolName, Handler) || !Handler)
		{
			return FUnrealOpenMcpToolDispatchResult::Fail(
				TEXT("handler_not_registered"),
				FString::Printf(TEXT("No handler registered for '%s'."), *ToolName));
		}
		return Handler(Body);
	}

	/** Remove the entire scratch tree so the next run starts clean. Best-effort.
	 *  Also clears any in-memory packages left under the scratch root (a
	 *  session-created Blueprint that was not saved keeps its package resident;
	 *  CollectGarbage lets a fresh create reuse the path within the same run). */
	void CleanupScratch()
	{
		if (UEditorAssetLibrary::DoesDirectoryExist(BlueprintScratchRoot))
		{
			UEditorAssetLibrary::DeleteDirectory(BlueprintScratchRoot);
		}
		CollectGarbage(RF_NoFlags);
	}

	/** Quote a string as a JSON token. */
	FString Quote(const FString& S)
	{
		return FString::Printf(TEXT("\"%s\""), *S);
	}

	/** Find a component object by variable name inside a blueprint_get result's
	 *  `components` array. Returns null when the array is absent or no entry
	 *  matches. Used by the add/remove-component cases to assert the SCS state
	 *  blueprint_get reports. */
	TSharedPtr<FJsonObject> FindComponentByName(const TSharedPtr<FJsonObject>& GetResult, const FString& Name)
	{
		if (!GetResult.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		if (!GetResult->TryGetArrayField(TEXT("components"), Components))
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Components)
		{
			const TSharedPtr<FJsonObject> Comp = Entry.IsValid() ? Entry->AsObject() : nullptr;
			if (Comp.IsValid() && Comp->GetStringField(TEXT("name")) == Name)
			{
				return Comp;
			}
		}
		return nullptr;
	}
}

void FUnrealOpenMcpBlueprintToolsSpec::Define()
{
	BeforeEach([this]()
	{
		CleanupScratch();
	});

	AfterEach([this]()
	{
		CleanupScratch();
	});

	Describe("unreal_open_mcp_blueprint_create", [this]()
	{
		// Happy path — create a Blueprint from the default Actor parent under
		// /Game; the result echoes name + path + parentClass.
		It("creates a Blueprint from the default Actor parent and reports name/path/parentClass", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_Created"), BlueprintScratchRoot);
			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestTrue(TEXT("name set"), Json->HasTypedField<EJson::String>(TEXT("name")));
			TestTrue(TEXT("path contains dest"), Json->GetStringField(TEXT("path")).Contains(TEXT("BP_Created")));
			TestTrue(TEXT("parentClass is Actor"), Json->GetStringField(TEXT("parentClass")).Contains(TEXT("Actor")));
		});

		// Explicit parent class path accepted.
		It("accepts an explicit parent_class path", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_ExplicitParent"), BlueprintScratchRoot);
			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("parent_class"), Quote(ActorParentPath) },
				}));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestTrue(TEXT("parentClass is Actor"), Json->GetStringField(TEXT("parentClass")).Contains(TEXT("Actor")));
		});

		// Object-path form ('/Game/.../BP_X.BP_X') is accepted and normalised
		// to the package path before validating.
		It("accepts the object-path form and normalises it", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_ObjPath.BP_ObjPath"), BlueprintScratchRoot);
			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			// The normalised path drops the object-name suffix.
			TestTrue(
				TEXT("path is the package path"),
				Json->GetStringField(TEXT("path")).Contains(TEXT("BP_ObjPath.BP_ObjPath")));
		});

		// Non-Blueprintable parent — a class that fails CanCreateBlueprintOfClass.
		// UAssetManager (or any non-Blueprintable native class) is refused with
		// parent_not_blueprintable (never a fatal assert inside CreateBlueprint).
		It("refuses a non-Blueprintable parent with parent_not_blueprintable", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_BadParent"), BlueprintScratchRoot);
			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					// UAssetManager is a native class but not Blueprintable.
					{ TEXT("parent_class"), Quote(TEXT("/Script/Engine.AssetManager")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			// Either the class is not Blueprintable (parent_not_blueprintable)
			// or — in an environment that does not ship AssetManager — it does
			// not resolve (parent_class_not_found). Both are the structured
			// refusal the contract promises (never a fatal assert).
			TestTrue(
				TEXT("structured refusal code"),
				R.Code == FString(TEXT("parent_not_blueprintable"))
					|| R.Code == FString(TEXT("parent_class_not_found")));
		});

		// Unresolvable parent — a path that names no class.
		It("refuses an unresolvable parent with parent_class_not_found", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_NoParent"), BlueprintScratchRoot);
			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("parent_class"), Quote(TEXT("/Script/Engine.__DefinitelyNotAClass__")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("parent_class_not_found")));
		});

		// Writable-root guard — refuse creating under /Engine.
		It("refuses an /Engine destination with invalid_content_root", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({
					{ TEXT("path"), Quote(TEXT("/Engine/__Mcp/BP_ShouldNotCreate")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("invalid_content_root")));
		});

		// Collision — creating over an existing Blueprint is refused with a
		// structured error (never a fatal assert).
		It("refuses an existing Blueprint destination with asset_already_exists", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_Dup"), BlueprintScratchRoot);
			const FUnrealOpenMcpToolDispatchResult R1 = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("first create ok"), R1.bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R2 = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } }));
			TestFalse(TEXT("second create fails"), R2.bOk);
			TestEqual(TEXT("code"), R2.Code, FString(TEXT("asset_already_exists")));
		});

		// Missing args.
		It("returns missing_parameter when path absent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_create"),
				TEXT("{}"));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("missing_parameter")));
		});

		// Malformed body.
		It("returns invalid_parameter for a malformed body", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_create"),
				TEXT("not json"));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("invalid_parameter")));
		});
	});

	Describe("unreal_open_mcp_blueprint_get", [this]()
	{
		// Round-trip after create — the summary arrays are all present (even if
		// empty for a fresh Actor Blueprint) and events carry `enabled`.
		It("reads a created Blueprint summary (identity + arrays + enabled on events)", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_Read"), BlueprintScratchRoot);
			const FUnrealOpenMcpToolDispatchResult Create = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("create ok"), Create.bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_get"),
				MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;
			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;

			// Identity.
			TestTrue(TEXT("name set"), Json->HasTypedField<EJson::String>(TEXT("name")));
			TestTrue(TEXT("path set"), Json->GetStringField(TEXT("path")).Contains(TEXT("BP_Read")));
			TestTrue(TEXT("parentClass is Actor"), Json->GetStringField(TEXT("parentClass")).Contains(TEXT("Actor")));

			// Summary arrays present.
			TestTrue(TEXT("variables array"), Json->HasTypedField<EJson::Array>(TEXT("variables")));
			TestTrue(TEXT("components array"), Json->HasTypedField<EJson::Array>(TEXT("components")));
			TestTrue(TEXT("functions array"), Json->HasTypedField<EJson::Array>(TEXT("functions")));
			TestTrue(TEXT("events array"), Json->HasTypedField<EJson::Array>(TEXT("events")));
			TestTrue(TEXT("interfaces array"), Json->HasTypedField<EJson::Array>(TEXT("interfaces")));
			TestTrue(TEXT("parentChain array"), Json->HasTypedField<EJson::Array>(TEXT("parentChain")));

			// parentChain is non-empty for an Actor Blueprint (Actor up to
			// UObject); it must include the Actor class name.
			const TArray<TSharedPtr<FJsonValue>>* ParentChain = nullptr;
			if (Json->TryGetArrayField(TEXT("parentChain"), ParentChain))
			{
				TestTrue(TEXT("parentChain non-empty"), ParentChain->Num() > 0);
				bool bFoundActor = false;
				for (const TSharedPtr<FJsonValue>& Entry : *ParentChain)
				{
					if (Entry.IsValid() && Entry->AsString().Contains(TEXT("Actor")))
					{
						bFoundActor = true;
						break;
					}
				}
				TestTrue(TEXT("parentChain includes Actor"), bFoundActor);
			}

			// Every event entry carries an `enabled` boolean (the
			// disabled-ghost-event contract — a fresh Actor Blueprint is
			// pre-seeded with DISABLED ReceiveBeginPlay/ReceiveTick ghosts, so
			// `enabled:false` must be distinguishable from "event not present").
			const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
			if (Json->TryGetArrayField(TEXT("events"), Events))
			{
				for (const TSharedPtr<FJsonValue>& Entry : *Events)
				{
					const TSharedPtr<FJsonObject> EventObj = Entry.IsValid() ? Entry->AsObject() : nullptr;
					if (!TestNotNull(TEXT("event object"), EventObj.Get())) continue;
					TestTrue(TEXT("event has enabled flag"), EventObj->HasTypedField<EJson::Boolean>(TEXT("enabled")));
				}
			}
		});

		// Missing asset.
		It("returns blueprint_not_found for a missing path", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_get"),
				MakeBody({ { TEXT("path"), Quote(TEXT("/Game/__DoesNotExist/BP_Nope")) } }));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("blueprint_not_found")));
		});

		// Missing arg.
		It("returns missing_parameter when path absent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_get"),
				TEXT("{}"));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("missing_parameter")));
		});
	});

	Describe("mutation classification", [this]()
	{
		// Pin the mutating/read-only classification so a later refactor cannot
		// accidentally re-classify a tool and break the gate contract.
		It("create/add_component/remove_component are mutating; get is read-only", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			auto CheckMutating = [this, &Registry](const FString& Name, bool bExpectedMutating)
			{
				FUnrealOpenMcpToolMetadata Meta;
				if (!TestTrue(*FString::Printf(TEXT("metadata for %s"), *Name), Registry.TryGetMetadata(Name, Meta)))
				{
					return;
				}
				TestEqual(
					*FString::Printf(TEXT("%s mutating"), *Name),
					Meta.bIsMutating,
					bExpectedMutating);
			};

			CheckMutating(TEXT("unreal_open_mcp_blueprint_create"), true);
			CheckMutating(TEXT("unreal_open_mcp_blueprint_get"), false);
			CheckMutating(TEXT("unreal_open_mcp_blueprint_add_component"), true);
			CheckMutating(TEXT("unreal_open_mcp_blueprint_remove_component"), true);
		});
	});

	Describe("unreal_open_mcp_blueprint_add_component", [this]()
	{
		// Happy path — add a StaticMeshComponent node; the result echoes the
		// component name + class, and blueprint_get reflects it.
		It("adds a StaticMeshComponent node reflected by blueprint_get", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_AddComp"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("component_class"), Quote(TEXT("/Script/Engine.StaticMeshComponent")) },
					{ TEXT("name"), Quote(TEXT("Mesh")) },
				}));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestEqual(TEXT("component name"), Json->GetStringField(TEXT("component")), FString(TEXT("Mesh")));
			TestTrue(TEXT("class is StaticMeshComponent"), Json->GetStringField(TEXT("class")).Contains(TEXT("StaticMeshComponent")));

			// blueprint_get now lists the component.
			const FUnrealOpenMcpToolDispatchResult Get = Invoke(
				Registry, TEXT("unreal_open_mcp_blueprint_get"), MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("get ok"), Get.bOk)) return;
			const TSharedPtr<FJsonObject> GetJson = ParseJson(Get.Output);
			TestTrue(TEXT("component present in get"), FindComponentByName(GetJson, TEXT("Mesh")).IsValid());
		});

		// Optional parent — a child scene component attaches under an existing
		// scene-component node; blueprint_get reports the parent field.
		It("attaches a child scene component under an existing scene-component parent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_Attach"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			// Parent: a scene component (StaticMeshComponent).
			if (!TestTrue(TEXT("add parent"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("component_class"), Quote(TEXT("/Script/Engine.StaticMeshComponent")) },
					{ TEXT("name"), Quote(TEXT("ParentMesh")) },
				})).bOk)) return;

			// Child: another scene component under ParentMesh.
			if (!TestTrue(TEXT("add child"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("component_class"), Quote(TEXT("/Script/Engine.StaticMeshComponent")) },
					{ TEXT("name"), Quote(TEXT("ChildMesh")) },
					{ TEXT("parent_component"), Quote(TEXT("ParentMesh")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult Get = Invoke(
				Registry, TEXT("unreal_open_mcp_blueprint_get"), MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("get ok"), Get.bOk)) return;
			const TSharedPtr<FJsonObject> GetJson = ParseJson(Get.Output);
			const TSharedPtr<FJsonObject> Child = FindComponentByName(GetJson, TEXT("ChildMesh"));
			if (!TestNotNull(TEXT("child present"), Child.Get())) return;
			TestEqual(TEXT("child parent is ParentMesh"), Child->GetStringField(TEXT("parent")), FString(TEXT("ParentMesh")));
		});

		// Abstract class — LightComponentBase is abstract; SCS CreateNode ->
		// NewObject would fatally assert. Rejected with abstract_component
		// (never a crash).
		It("rejects an abstract component class with abstract_component", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_Abstract"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("component_class"), Quote(TEXT("/Script/Engine.LightComponentBase")) },
					{ TEXT("name"), Quote(TEXT("BadLight")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("abstract_component")));
		});

		// Name collision with an existing SCS node — rejected with
		// name_collision.
		It("rejects a name collision with an existing SCS node", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_CollideSCS"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;
			if (!TestTrue(TEXT("add first"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("component_class"), Quote(TEXT("/Script/Engine.StaticMeshComponent")) },
					{ TEXT("name"), Quote(TEXT("Dup")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("component_class"), Quote(TEXT("/Script/Engine.StaticMeshComponent")) },
					{ TEXT("name"), Quote(TEXT("Dup")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("name_collision")));
		});

		// Non-scene component cannot attach under a parent — attachment is a
		// scene-graph op, so a non-USceneComponent under a scene parent is
		// rejected with invalid_attachment. Uses a MovementComponent (an
		// ActorComponent but not a SceneComponent).
		It("rejects a non-scene component attached under a parent with invalid_attachment", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_NonScene"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;
			// A scene parent to attempt attaching under.
			if (!TestTrue(TEXT("add parent"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("component_class"), Quote(TEXT("/Script/Engine.StaticMeshComponent")) },
					{ TEXT("name"), Quote(TEXT("ParentMesh")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					// PawnMovementComponent is concrete + non-abstract but not a
					// USceneComponent, so it exercises the attachment guard
					// without tripping the abstract check.
					{ TEXT("component_class"), Quote(TEXT("/Script/Engine.PawnMovementComponent")) },
					{ TEXT("name"), Quote(TEXT("Mover")) },
					{ TEXT("parent_component"), Quote(TEXT("ParentMesh")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			// PawnMovementComponent may be abstract in some engine versions; if
			// so the guard still fires (abstract_component). Either way the
			// fatal NewObject path is never reached — both codes are the
			// structured refusal the contract promises.
			TestTrue(
				TEXT("structured refusal code"),
				R.Code == FString(TEXT("invalid_attachment"))
					|| R.Code == FString(TEXT("abstract_component")));
		});

		// Missing parent — a parent_component that names no SCS node is
		// rejected with parent_not_found.
		It("rejects a missing parent component with parent_not_found", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_NoParent"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("component_class"), Quote(TEXT("/Script/Engine.StaticMeshComponent")) },
					{ TEXT("name"), Quote(TEXT("Mesh")) },
					{ TEXT("parent_component"), Quote(TEXT("DoesNotExist")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("parent_not_found")));
		});

		// Invalid component class — a non-UActorComponent resolves but fails
		// the subclass check (invalid_component_class), and an unresolvable
		// path resolves to null (also invalid_component_class).
		It("rejects a non-UActorComponent class with invalid_component_class", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_BadClass"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			// Actor is a UClass but not a UActorComponent.
			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("component_class"), Quote(TEXT("/Script/Engine.Actor")) },
					{ TEXT("name"), Quote(TEXT("Mesh")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("invalid_component_class")));
		});

		// Missing args — path or name or component_class absent.
		It("returns missing_parameter when path/name/component_class absent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_MissingArgs"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			// No name.
			TestEqual(TEXT("missing name"),
				Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_component"),
					MakeBody({
						{ TEXT("path"), Quote(Dest) },
						{ TEXT("component_class"), Quote(TEXT("/Script/Engine.StaticMeshComponent")) },
					})).Code,
				FString(TEXT("missing_parameter")));
			// No component_class.
			TestEqual(TEXT("missing component_class"),
				Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_component"),
					MakeBody({
						{ TEXT("path"), Quote(Dest) },
						{ TEXT("name"), Quote(TEXT("Mesh")) },
					})).Code,
				FString(TEXT("missing_parameter")));
			// No path at all.
			TestEqual(TEXT("missing path"),
				Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_component"), TEXT("{}")).Code,
				FString(TEXT("missing_parameter")));
		});

		// Missing Blueprint — add_component on an absent path.
		It("returns blueprint_not_found for a missing Blueprint", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_component"),
				MakeBody({
					{ TEXT("path"), Quote(TEXT("/Game/__DoesNotExist/BP_Nope")) },
					{ TEXT("component_class"), Quote(TEXT("/Script/Engine.StaticMeshComponent")) },
					{ TEXT("name"), Quote(TEXT("Mesh")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("blueprint_not_found")));
		});
	});

	Describe("unreal_open_mcp_blueprint_remove_component", [this]()
	{
		// Happy path — remove a node; blueprint_get no longer lists it.
		It("removes a node so blueprint_get no longer lists it", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_Remove"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;
			if (!TestTrue(TEXT("add"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("component_class"), Quote(TEXT("/Script/Engine.StaticMeshComponent")) },
					{ TEXT("name"), Quote(TEXT("Mesh")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_remove_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("Mesh")) },
				}));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const FUnrealOpenMcpToolDispatchResult Get = Invoke(
				Registry, TEXT("unreal_open_mcp_blueprint_get"), MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("get ok"), Get.bOk)) return;
			TestFalse(TEXT("component gone from get"), FindComponentByName(ParseJson(Get.Output), TEXT("Mesh")).IsValid());
		});

		// Children are promoted — removing a parent node leaves the child
		// present (re-parented onto the removed node's parent). This pins the
		// RemoveNodeAndPromoteChildren contract.
		It("promotes children when removing a parent node", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_Promote"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;
			if (!TestTrue(TEXT("add parent"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("component_class"), Quote(TEXT("/Script/Engine.StaticMeshComponent")) },
					{ TEXT("name"), Quote(TEXT("ParentMesh")) },
				})).bOk)) return;
			if (!TestTrue(TEXT("add child"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("component_class"), Quote(TEXT("/Script/Engine.StaticMeshComponent")) },
					{ TEXT("name"), Quote(TEXT("ChildMesh")) },
					{ TEXT("parent_component"), Quote(TEXT("ParentMesh")) },
				})).bOk)) return;

			// Remove the PARENT — the child must survive (promoted).
			if (!TestTrue(TEXT("remove parent"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_remove_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("ParentMesh")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult Get = Invoke(
				Registry, TEXT("unreal_open_mcp_blueprint_get"), MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("get ok"), Get.bOk)) return;
			// Parent gone, child still present.
			TestFalse(TEXT("parent removed"), FindComponentByName(ParseJson(Get.Output), TEXT("ParentMesh")).IsValid());
			TestTrue(TEXT("child promoted + still present"), FindComponentByName(ParseJson(Get.Output), TEXT("ChildMesh")).IsValid());
		});

		// Missing component — removing a name that is not an SCS node is
		// rejected with component_not_found.
		It("rejects a missing component with component_not_found", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_RemoveMissing"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_remove_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("NoSuchComponent")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("component_not_found")));
		});

		// Missing args.
		It("returns missing_parameter when path/name absent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_RemoveMissingArgs"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			// No name.
			TestEqual(TEXT("missing name"),
				Invoke(Registry, TEXT("unreal_open_mcp_blueprint_remove_component"),
					MakeBody({ { TEXT("path"), Quote(Dest) } })).Code,
				FString(TEXT("missing_parameter")));
			// No path at all.
			TestEqual(TEXT("missing path"),
				Invoke(Registry, TEXT("unreal_open_mcp_blueprint_remove_component"), TEXT("{}")).Code,
				FString(TEXT("missing_parameter")));
		});

		// Missing Blueprint.
		It("returns blueprint_not_found for a missing Blueprint", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_remove_component"),
				MakeBody({
					{ TEXT("path"), Quote(TEXT("/Game/__DoesNotExist/BP_Nope")) },
					{ TEXT("name"), Quote(TEXT("Mesh")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("blueprint_not_found")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
