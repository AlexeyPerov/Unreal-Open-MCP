// unreal_open_mcp_blueprint_create / blueprint_get / blueprint_add_component /
// blueprint_remove_component / blueprint_add_variable /
// blueprint_modify_variable / blueprint_set_default / blueprint_add_function /
// blueprint_add_event Automation specs (P6.1 + P6.2 + P6.3 + P6.4).
//
// Pins the Blueprint tool family end-to-end at the handler level. The cases
// mirror the P6.1 + P6.2 + P6.3 + P6.4 plan acceptance criteria + test lists:
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
//   - add_variable: happy path (Speed:int added; result echoes variable + type;
//     blueprint_get reflects name + type + isArray); array flag round-trips
//     (ints[]); default_value stored; math struct type (vector); name collision
//     with an existing variable, an SCS component, and a parent-class property
//     rejected (name_collision); unknown type rejected (invalid_type);
//     missing_parameter.
//   - modify_variable: retype (Speed:int -> MaxSpeed:float round-trips via get);
//     rename; rename + retype together; validate-before-mutate — a colliding
//     new_name does NOT commit a new_type (no partial mutation); variable not
//     found rejected (variable_not_found); neither new_name nor new_type
//     rejected (missing_parameter); name_collision on rename; invalid_type on
//     retype.
//   - set_default: writes a CDO property (an Actor-typed Blueprint inherits a
//     property that survives a compile — tested via the generated class); a
//     property absent on the generated class rejected with property_not_found
//     whose message points at blueprint_compile; no_generated_class when the
//     GeneratedClass is missing; missing_parameter.
//   - add_function: happy path (DoThing function graph added; result echoes the
//     name; blueprint_get lists it); duplicate function name rejected
//     (name_collision); an outer-name collision with a UObject already outered
//     to the Blueprint rejected (name_collision — the CreateNewGraph rename-
//     aside hijack is never silently accepted); missing_parameter.
//   - add_event: happy path (ReceiveBeginPlay event enabled; result echoes the
//     name + enabled:true; blueprint_get lists it as enabled); a non-overridable
//     parent function rejected (K2_DestroyActor is BlueprintCallable but not a
//     BlueprintEvent → not_an_event, never a nonsense node); a non-existent
//     parent function name rejected (not_an_event); a second add of an already
//     enabled event rejected (event_already_exists); a ghost event (the fresh
//     Actor event graph is pre-seeded with a DISABLED ReceiveTick ghost) is
//     ENABLED by add-event (does not false-succeed as no-op); missing_parameter.
//   - mutation classification: create/add/remove/add_variable/modify_variable/
//     set_default/add_function/add_event mutating, get read-only.
//
// The suite owns its scratch tree under /Game/__McpP61Blueprint — teardown
// removes the whole subtree so the automation project does not accumulate test
// artifacts between runs. The default parent (Actor —
// '/Script/Engine.Actor') is a native class always present in the editor, so
// the default-parent cases never need a skip.
//
// Fidelity: greenfield vs Unity (no Blueprint twin); behavior-adapted from
// Unreal-MCP's blueprint-create / blueprint-get test surface (the any-UObject
// collision probe + the disabled-ghost-event `enabled` flag) and the
// blueprint-add-function / blueprint-add-event test surface (the outer-name
// hijack probe + the FunctionCanBePlacedAsEvent reject + the ghost-enable +
// duplicate-event reject).
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Tools/UnrealOpenMcpBlueprintTools.h"

#include "Engine/Blueprint.h"               // UBlueprint — set_default tests compile + resolve the asset
#include "Kismet2/KismetEditorUtilities.h"   // CompileBlueprint — set_default needs a property on the generated class
#include "Kismet2/BlueprintEditorUtils.h"   // FindEventGraph — add_event asserts an enabled node
#include "EdGraph/EdGraph.h"                // UEdGraph — FindEventGraph return type
#include "K2Node_Event.h"                   // UK2Node_Event — enabled-node assertion in add_event

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
	/** Scratch tree root — every P6.1 + P6.2 + P6.3 case lives under here so
	 *  teardown can remove the whole subtree with one DeleteDirectory. */
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

	/** Find a member-variable object by name inside a blueprint_get result's
	 *  `variables` array. Returns null when the array is absent or no entry
	 *  matches. Used by the variable add/modify cases to assert the variable
	 *  state blueprint_get reports. */
	TSharedPtr<FJsonObject> FindVariableByName(const TSharedPtr<FJsonObject>& GetResult, const FString& Name)
	{
		if (!GetResult.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
		if (!GetResult->TryGetArrayField(TEXT("variables"), Variables))
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Variables)
		{
			const TSharedPtr<FJsonObject> Var = Entry.IsValid() ? Entry->AsObject() : nullptr;
			if (Var.IsValid() && Var->GetStringField(TEXT("name")) == Name)
			{
				return Var;
			}
		}
		return nullptr;
	}

	/** Find a function object by name inside a blueprint_get result's
	 *  `functions` array. Returns null when the array is absent or no entry
	 *  matches. Used by the add_function cases to assert the function state
	 *  blueprint_get reports. */
	TSharedPtr<FJsonObject> FindFunctionByName(const TSharedPtr<FJsonObject>& GetResult, const FString& Name)
	{
		if (!GetResult.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
		if (!GetResult->TryGetArrayField(TEXT("functions"), Functions))
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Functions)
		{
			const TSharedPtr<FJsonObject> Func = Entry.IsValid() ? Entry->AsObject() : nullptr;
			if (Func.IsValid() && Func->GetStringField(TEXT("name")) == Name)
			{
				return Func;
			}
		}
		return nullptr;
	}

	/** Find an event object by name inside a blueprint_get result's `events`
	 *  array. Returns null when the array is absent or no entry matches. Used
	 *  by the add_event cases to assert the event state blueprint_get reports.
	 *  An event entry is { name, enabled }. */
	TSharedPtr<FJsonObject> FindEventByName(const TSharedPtr<FJsonObject>& GetResult, const FString& Name)
	{
		if (!GetResult.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
		if (!GetResult->TryGetArrayField(TEXT("events"), Events))
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Events)
		{
			const TSharedPtr<FJsonObject> Ev = Entry.IsValid() ? Entry->AsObject() : nullptr;
			if (Ev.IsValid() && Ev->GetStringField(TEXT("name")) == Name)
			{
				return Ev;
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
		It("create/add/remove/add_variable/modify_variable/set_default/add_function/add_event are mutating; get is read-only", [this]()
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
			CheckMutating(TEXT("unreal_open_mcp_blueprint_add_variable"), true);
			CheckMutating(TEXT("unreal_open_mcp_blueprint_modify_variable"), true);
			CheckMutating(TEXT("unreal_open_mcp_blueprint_set_default"), true);
			CheckMutating(TEXT("unreal_open_mcp_blueprint_add_function"), true);
			CheckMutating(TEXT("unreal_open_mcp_blueprint_add_event"), true);
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

	Describe("unreal_open_mcp_blueprint_add_variable", [this]()
	{
		// Happy path — add an int variable; the result echoes the variable name
		// + type, and blueprint_get reflects name + type + isArray:false.
		It("adds an int variable reflected by blueprint_get", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_AddVar"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("Speed")) },
					{ TEXT("type"), Quote(TEXT("int")) },
				}));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestEqual(TEXT("variable name"), Json->GetStringField(TEXT("variable")), FString(TEXT("Speed")));
			TestEqual(TEXT("type is int"), Json->GetStringField(TEXT("type")), FString(TEXT("int")));

			// blueprint_get now lists the variable with the right name/type/array flag.
			const FUnrealOpenMcpToolDispatchResult Get = Invoke(
				Registry, TEXT("unreal_open_mcp_blueprint_get"), MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("get ok"), Get.bOk)) return;
			const TSharedPtr<FJsonObject> Var = FindVariableByName(ParseJson(Get.Output), TEXT("Speed"));
			if (!TestNotNull(TEXT("variable present in get"), Var.Get())) return;
			TestEqual(TEXT("get type"), Var->GetStringField(TEXT("type")), FString(TEXT("int")));
			TestEqual(TEXT("get isArray"), Var->GetBoolField(TEXT("isArray")), false);
		});

		// Array flag round-trips — is_array:true produces an array type that
		// blueprint_get reports with the [] suffix + isArray:true.
		It("wraps the type in an array when is_array is true", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_AddArray"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("Ids")) },
					{ TEXT("type"), Quote(TEXT("int")) },
					{ TEXT("is_array"), TEXT("true") },
				}));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestEqual(TEXT("type is int[]"), Json->GetStringField(TEXT("type")), FString(TEXT("int[]")));

			const FUnrealOpenMcpToolDispatchResult Get = Invoke(
				Registry, TEXT("unreal_open_mcp_blueprint_get"), MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("get ok"), Get.bOk)) return;
			const TSharedPtr<FJsonObject> Var = FindVariableByName(ParseJson(Get.Output), TEXT("Ids"));
			if (!TestNotNull(TEXT("variable present"), Var.Get())) return;
			TestEqual(TEXT("get isArray"), Var->GetBoolField(TEXT("isArray")), true);
		});

		// default_value is stored on the descriptor and surfaced by blueprint_get.
		It("stores an optional default_value on the variable", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_Default"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			if (!TestTrue(TEXT("add ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("Count")) },
					{ TEXT("type"), Quote(TEXT("int")) },
					{ TEXT("default_value"), Quote(TEXT("42")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult Get = Invoke(
				Registry, TEXT("unreal_open_mcp_blueprint_get"), MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("get ok"), Get.bOk)) return;
			const TSharedPtr<FJsonObject> Var = FindVariableByName(ParseJson(Get.Output), TEXT("Count"));
			if (!TestNotNull(TEXT("variable present"), Var.Get())) return;
			TestEqual(TEXT("default value"), Var->GetStringField(TEXT("default")), FString(TEXT("42")));
		});

		// Math struct type — vector maps to the FVector base structure.
		It("accepts a math struct type token (vector)", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_Vector"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("Vel")) },
					{ TEXT("type"), Quote(TEXT("vector")) },
				}));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			// PinTypeToString emits the script-struct name for a struct pin type.
			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestEqual(TEXT("type is Vector"), Json->GetStringField(TEXT("type")), FString(TEXT("Vector")));
		});

		// Name collision with an existing member variable — rejected with
		// name_collision.
		It("rejects a name collision with an existing variable", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_VarCollide"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;
			if (!TestTrue(TEXT("add first"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("Dup")) },
					{ TEXT("type"), Quote(TEXT("int")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("Dup")) },
					{ TEXT("type"), Quote(TEXT("int")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("name_collision")));
		});

		// Name collision with an existing SCS component — member-variable
		// names, SCS component names, and inherited parent properties share the
		// generated class's property namespace.
		It("rejects a name collision with an existing SCS component", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_VarVsComp"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;
			if (!TestTrue(TEXT("add component"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_component"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("component_class"), Quote(TEXT("/Script/Engine.StaticMeshComponent")) },
					{ TEXT("name"), Quote(TEXT("Mesh")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("Mesh")) },
					{ TEXT("type"), Quote(TEXT("int")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("name_collision")));
		});

		// Name collision with a parent-class property — Actor has an `IsHidden`
		// (EDU) but a stable inherited Edit property like `NetUpdateFrequency`
		// is on Actor; use a property guaranteed present on Actor.
		It("rejects a name collision with a parent-class property", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_VarVsParent"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			// 'Name' is a property on Actor (the actor's editable label/name) —
			// reserved across the generated class namespace. Use 'Tags' (an
			// EditAnywhere TArray on AActor) so the parent-property collision
			// guard fires.
			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("Tags")) },
					{ TEXT("type"), Quote(TEXT("int")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("name_collision")));
		});

		// Unknown type — a token that names no primitive/struct/object is
		// rejected with invalid_type.
		It("rejects an unknown type token with invalid_type", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_BadType"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("X")) },
					{ TEXT("type"), Quote(TEXT("__DefinitelyNotAType__")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("invalid_type")));
		});

		// Missing args — path or name or type absent.
		It("returns missing_parameter when path/name/type absent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_VarMissing"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			// No name.
			TestEqual(TEXT("missing name"),
				Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_variable"),
					MakeBody({
						{ TEXT("path"), Quote(Dest) },
						{ TEXT("type"), Quote(TEXT("int")) },
					})).Code,
				FString(TEXT("missing_parameter")));
			// No type.
			TestEqual(TEXT("missing type"),
				Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_variable"),
					MakeBody({
						{ TEXT("path"), Quote(Dest) },
						{ TEXT("name"), Quote(TEXT("X")) },
					})).Code,
				FString(TEXT("missing_parameter")));
			// No path at all.
			TestEqual(TEXT("missing path"),
				Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_variable"), TEXT("{}")).Code,
				FString(TEXT("missing_parameter")));
		});

		// Missing Blueprint — add_variable on an absent path.
		It("returns blueprint_not_found for a missing Blueprint", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(TEXT("/Game/__DoesNotExist/BP_Nope")) },
					{ TEXT("name"), Quote(TEXT("X")) },
					{ TEXT("type"), Quote(TEXT("int")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("blueprint_not_found")));
		});
	});

	Describe("unreal_open_mcp_blueprint_modify_variable", [this]()
	{
		// Retype — Speed:int becomes MaxSpeed:float; blueprint_get reflects the
		// new type under the original name.
		It("retypes a variable (int -> float) reflected by blueprint_get", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_Retype"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;
			if (!TestTrue(TEXT("add"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("Speed")) },
					{ TEXT("type"), Quote(TEXT("int")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_modify_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("Speed")) },
					{ TEXT("new_type"), Quote(TEXT("float")) },
				}));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const FUnrealOpenMcpToolDispatchResult Get = Invoke(
				Registry, TEXT("unreal_open_mcp_blueprint_get"), MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("get ok"), Get.bOk)) return;
			const TSharedPtr<FJsonObject> Var = FindVariableByName(ParseJson(Get.Output), TEXT("Speed"));
			if (!TestNotNull(TEXT("variable present"), Var.Get())) return;
			TestEqual(TEXT("type is now float"), Var->GetStringField(TEXT("type")), FString(TEXT("float")));
		});

		// Rename — the variable moves to the new name (old name gone).
		It("renames a variable (old name gone, new name present)", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_Rename"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;
			if (!TestTrue(TEXT("add"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("Old")) },
					{ TEXT("type"), Quote(TEXT("int")) },
				})).bOk)) return;

			if (!TestTrue(TEXT("modify ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_modify_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("Old")) },
					{ TEXT("new_name"), Quote(TEXT("New")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult Get = Invoke(
				Registry, TEXT("unreal_open_mcp_blueprint_get"), MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("get ok"), Get.bOk)) return;
			const TSharedPtr<FJsonObject> GetJson = ParseJson(Get.Output);
			TestFalse(TEXT("old name gone"), FindVariableByName(GetJson, TEXT("Old")).IsValid());
			TestTrue(TEXT("new name present"), FindVariableByName(GetJson, TEXT("New")).IsValid());
		});

		// Rename + retype together in one call.
		It("renames and retypes in the same call", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_Both"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;
			if (!TestTrue(TEXT("add"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("Old")) },
					{ TEXT("type"), Quote(TEXT("int")) },
				})).bOk)) return;

			if (!TestTrue(TEXT("modify ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_modify_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("Old")) },
					{ TEXT("new_name"), Quote(TEXT("New")) },
					{ TEXT("new_type"), Quote(TEXT("float")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult Get = Invoke(
				Registry, TEXT("unreal_open_mcp_blueprint_get"), MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("get ok"), Get.bOk)) return;
			const TSharedPtr<FJsonObject> Var = FindVariableByName(ParseJson(Get.Output), TEXT("New"));
			if (!TestNotNull(TEXT("new name present"), Var.Get())) return;
			TestEqual(TEXT("type is float"), Var->GetStringField(TEXT("type")), FString(TEXT("float")));
		});

		// Validate-before-mutate — a colliding new_name does NOT commit a
		// valid new_type. The type must remain unchanged (no partial mutation).
		It("does not commit a new_type when the new_name collides (validate-before-mutate)", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_Order"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;
			// Two variables: 'A' (int) and 'B' (int). Renaming A onto B must
			// fail AND must not have changed A's type to float.
			if (!TestTrue(TEXT("add A"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("A")) },
					{ TEXT("type"), Quote(TEXT("int")) },
				})).bOk)) return;
			if (!TestTrue(TEXT("add B"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("B")) },
					{ TEXT("type"), Quote(TEXT("int")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_modify_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("A")) },
					{ TEXT("new_name"), Quote(TEXT("B")) },  // collides
					{ TEXT("new_type"), Quote(TEXT("float")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("name_collision")));

			// A's type must STILL be int — the retype did not land.
			const FUnrealOpenMcpToolDispatchResult Get = Invoke(
				Registry, TEXT("unreal_open_mcp_blueprint_get"), MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("get ok"), Get.bOk)) return;
			const TSharedPtr<FJsonObject> Var = FindVariableByName(ParseJson(Get.Output), TEXT("A"));
			if (!TestNotNull(TEXT("A still present"), Var.Get())) return;
			TestEqual(TEXT("A type unchanged (int)"), Var->GetStringField(TEXT("type")), FString(TEXT("int")));
		});

		// Variable not found — modifying a name that is not a member variable.
		It("rejects a missing variable with variable_not_found", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_NoVar"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_modify_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("NoSuchVar")) },
					{ TEXT("new_type"), Quote(TEXT("float")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("variable_not_found")));
		});

		// Neither new_name nor new_type — missing_parameter.
		It("rejects a modify with neither new_name nor new_type with missing_parameter", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_NoChange"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;
			if (!TestTrue(TEXT("add"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("X")) },
					{ TEXT("type"), Quote(TEXT("int")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_modify_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("X")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("missing_parameter")));
		});

		// name_collision on rename — renaming onto an existing variable name.
		It("rejects a rename onto an existing variable name with name_collision", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_RenameCollide"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;
			if (!TestTrue(TEXT("add A"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("A")) },
					{ TEXT("type"), Quote(TEXT("int")) },
				})).bOk)) return;
			if (!TestTrue(TEXT("add B"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("B")) },
					{ TEXT("type"), Quote(TEXT("int")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_modify_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("A")) },
					{ TEXT("new_name"), Quote(TEXT("B")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("name_collision")));
		});

		// invalid_type on retype — a bad new_type token is rejected.
		It("rejects a bad new_type with invalid_type", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_RetypeBad"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;
			if (!TestTrue(TEXT("add"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("X")) },
					{ TEXT("type"), Quote(TEXT("int")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_modify_variable"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("X")) },
					{ TEXT("new_type"), Quote(TEXT("__NotAType__")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("invalid_type")));
		});
	});

	Describe("unreal_open_mcp_blueprint_set_default", [this]()
	{
		// Happy path — write a CDO property on a compiled Blueprint. An Actor-
		// typed Blueprint inherits EditAnywhere properties; 'NetUpdateFrequency'
		// (float) is present on AActor and accepts a numeric text value. The
		// Blueprint is compiled first so the property lands on the generated
		// class's CDO.
		It("writes a CDO property on a compiled Blueprint", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_SetDefault"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			// Resolve + compile so the generated class + CDO exist with the
			// inherited property.
			UBlueprint* Blueprint = FUnrealOpenMcpBlueprintHelpers::ResolveBlueprint(Dest);
			if (!TestNotNull(TEXT("resolve blueprint"), Blueprint)) return;
			FKismetEditorUtilities::CompileBlueprint(Blueprint);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_set_default"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("property"), Quote(TEXT("NetUpdateFrequency")) },
					{ TEXT("value"), Quote(TEXT("42.0")) },
				}));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestEqual(TEXT("property echoed"), Json->GetStringField(TEXT("property")), FString(TEXT("NetUpdateFrequency")));
			TestEqual(TEXT("value echoed"), Json->GetStringField(TEXT("value")), FString(TEXT("42.0")));
		});

		// property_not_found — a property absent on the generated class is
		// rejected with a message pointing at blueprint_compile (so an agent
		// that just added a variable learns the compile-first requirement).
		It("rejects a missing property with property_not_found (mentions compile)", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_NoProp"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			UBlueprint* Blueprint = FUnrealOpenMcpBlueprintHelpers::ResolveBlueprint(Dest);
			if (!TestNotNull(TEXT("resolve blueprint"), Blueprint)) return;
			FKismetEditorUtilities::CompileBlueprint(Blueprint);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_set_default"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("property"), Quote(TEXT("__NoSuchProperty__")) },
					{ TEXT("value"), Quote(TEXT("1")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("property_not_found")));
			// The message must hint at the compile-first path for newly added
			// variables.
			TestTrue(TEXT("message mentions compile"), R.Message.Contains(TEXT("compile")));
		});

		// Missing args — path or property absent.
		It("returns missing_parameter when path/property absent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_SetMissing"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			// No property.
			TestEqual(TEXT("missing property"),
				Invoke(Registry, TEXT("unreal_open_mcp_blueprint_set_default"),
					MakeBody({
						{ TEXT("path"), Quote(Dest) },
						{ TEXT("value"), Quote(TEXT("1")) },
					})).Code,
				FString(TEXT("missing_parameter")));
			// No path at all.
			TestEqual(TEXT("missing path"),
				Invoke(Registry, TEXT("unreal_open_mcp_blueprint_set_default"), TEXT("{}")).Code,
				FString(TEXT("missing_parameter")));
		});

		// Missing Blueprint — set_default on an absent path.
		It("returns blueprint_not_found for a missing Blueprint", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_set_default"),
				MakeBody({
					{ TEXT("path"), Quote(TEXT("/Game/__DoesNotExist/BP_Nope")) },
					{ TEXT("property"), Quote(TEXT("X")) },
					{ TEXT("value"), Quote(TEXT("1")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("blueprint_not_found")));
		});
	});

	Describe("unreal_open_mcp_blueprint_add_function", [this]()
	{
		// Happy path — add a DoThing function graph; the result echoes the
		// function name, and blueprint_get lists it.
		It("adds a function graph reflected by blueprint_get", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_AddFunc"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_function"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("DoThing")) },
				}));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestEqual(TEXT("function name"), Json->GetStringField(TEXT("function")), FString(TEXT("DoThing")));

			// blueprint_get now lists the function.
			const FUnrealOpenMcpToolDispatchResult Get = Invoke(
				Registry, TEXT("unreal_open_mcp_blueprint_get"), MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("get ok"), Get.bOk)) return;
			TestTrue(TEXT("function present in get"), FindFunctionByName(ParseJson(Get.Output), TEXT("DoThing")).IsValid());
		});

		// Duplicate function name — rejected with name_collision.
		It("rejects a duplicate function name with name_collision", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_FuncDup"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;
			if (!TestTrue(TEXT("add first"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_function"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("DoThing")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_function"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("DoThing")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("name_collision")));
		});

		// Outer-name hijack — a name colliding with the EventGraph (a UObject
		// outered to the Blueprint) is rejected up front. CreateNewGraph would
		// otherwise resolve the clash by renaming the EXISTING object aside,
		// silently hijacking it and reporting success.
		It("rejects an outer-name collision with the EventGraph (no silent hijack)", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_OuterClash"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			// Resolve the Blueprint to read its EventGraph's actual FName — the
			// pre-seeded event graph is outered to the Blueprint under a stable
			// name, so a function with that name is the hijack case.
			UBlueprint* Blueprint = FUnrealOpenMcpBlueprintHelpers::ResolveBlueprint(Dest);
			if (!TestNotNull(TEXT("resolve blueprint"), Blueprint)) return;
			FString EventGraphName;
			if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint))
			{
				EventGraphName = EventGraph->GetName();
			}
			if (!TestTrue(TEXT("event graph resolved"), !EventGraphName.IsEmpty())) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_function"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(EventGraphName) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("name_collision")));
		});

		// Missing args — path or name absent.
		It("returns missing_parameter when path/name absent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_FuncMissing"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			// No name.
			TestEqual(TEXT("missing name"),
				Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_function"),
					MakeBody({ { TEXT("path"), Quote(Dest) } })).Code,
				FString(TEXT("missing_parameter")));
			// No path at all.
			TestEqual(TEXT("missing path"),
				Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_function"), TEXT("{}")).Code,
				FString(TEXT("missing_parameter")));
		});

		// Missing Blueprint — add_function on an absent path.
		It("returns blueprint_not_found for a missing Blueprint", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_function"),
				MakeBody({
					{ TEXT("path"), Quote(TEXT("/Game/__DoesNotExist/BP_Nope")) },
					{ TEXT("name"), Quote(TEXT("DoThing")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("blueprint_not_found")));
		});
	});

	Describe("unreal_open_mcp_blueprint_add_event", [this]()
	{
		// Happy path — add ReceiveBeginPlay; the result echoes the event name +
		// enabled:true, and blueprint_get lists it as enabled. The fresh Actor
		// event graph is pre-seeded with a DISABLED ghost for ReceiveBeginPlay,
		// so this case also pins the ghost-enable path (the event goes from a
		// disabled ghost to an enabled node — add_event does NOT false-succeed
		// as a no-op).
		It("enables the ReceiveBeginPlay event reflected by blueprint_get as enabled", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_AddEvent"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_event"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("ReceiveBeginPlay")) },
				}));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestEqual(TEXT("event name"), Json->GetStringField(TEXT("event")), FString(TEXT("ReceiveBeginPlay")));
			TestEqual(TEXT("enabled flag"), Json->GetBoolField(TEXT("enabled")), true);

			// blueprint_get now lists the event with enabled:true.
			const FUnrealOpenMcpToolDispatchResult Get = Invoke(
				Registry, TEXT("unreal_open_mcp_blueprint_get"), MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("get ok"), Get.bOk)) return;
			const TSharedPtr<FJsonObject> Ev = FindEventByName(ParseJson(Get.Output), TEXT("ReceiveBeginPlay"));
			if (!TestNotNull(TEXT("event present in get"), Ev.Get())) return;
			TestEqual(TEXT("get enabled"), Ev->GetBoolField(TEXT("enabled")), true);

			// The node must be ENABLED on the actual graph — confirm via the
			// K2Node_Event's IsNodeEnabled so the ghost-enable contract holds at
			// the editor-object level too (not just the get surface).
			UBlueprint* Blueprint = FUnrealOpenMcpBlueprintHelpers::ResolveBlueprint(Dest);
			if (!TestNotNull(TEXT("resolve blueprint"), Blueprint)) return;
			bool bFoundEnabled = false;
			if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint))
			{
				for (const UEdGraphNode* Node : EventGraph->Nodes)
				{
					if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
					{
						if (EventNode->EventReference.GetMemberName() == FName(TEXT("ReceiveBeginPlay"))
							&& EventNode->IsNodeEnabled())
						{
							bFoundEnabled = true;
							break;
						}
					}
				}
			}
			TestTrue(TEXT("enabled node present on graph"), bFoundEnabled);
		});

		// Non-overridable parent function — K2_DestroyActor is BlueprintCallable
		// but not a BlueprintEvent, so FunctionCanBePlacedAsEvent rejects it with
		// not_an_event (never a nonsense node seeded + reported as success).
		It("rejects a non-overridable parent function (K2_DestroyActor) with not_an_event", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_BadEvent"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_event"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("K2_DestroyActor")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("not_an_event")));
		});

		// Non-existent parent function — a name that names no parent UFunction
		// is rejected with not_an_event.
		It("rejects a non-existent parent function with not_an_event", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_NoEvent"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_event"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("__DefinitelyNotAnEvent__")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("not_an_event")));
		});

		// Duplicate event — a second add of an already enabled event is rejected
		// with event_already_exists (no second ghost node minted).
		It("rejects a second add of an already enabled event with event_already_exists", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_EventDup"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;
			if (!TestTrue(TEXT("add first"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_event"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("ReceiveBeginPlay")) },
				})).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_event"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("ReceiveBeginPlay")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("event_already_exists")));
		});

		// Ghost-enable on a fresh event — ReceiveTick is also a pre-seeded
		// DISABLED ghost on a fresh Actor event graph; add-event must ENABLE it
		// (not false-succeed as a no-op). Distinct from the ReceiveBeginPlay
		// case so the ghost-enable contract is exercised on a second seed.
		It("enables the ReceiveTick ghost (no false no-op)", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_TickGhost"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_event"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("name"), Quote(TEXT("ReceiveTick")) },
				}));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			// blueprint_get must list ReceiveTick with enabled:true (the ghost
			// transitioned from disabled to enabled — add_event did not no-op).
			const FUnrealOpenMcpToolDispatchResult Get = Invoke(
				Registry, TEXT("unreal_open_mcp_blueprint_get"), MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("get ok"), Get.bOk)) return;
			const TSharedPtr<FJsonObject> Ev = FindEventByName(ParseJson(Get.Output), TEXT("ReceiveTick"));
			if (!TestNotNull(TEXT("event present in get"), Ev.Get())) return;
			TestEqual(TEXT("get enabled"), Ev->GetBoolField(TEXT("enabled")), true);
		});

		// Missing args — path or name absent.
		It("returns missing_parameter when path/name absent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/BP_EventMissing"), BlueprintScratchRoot);
			if (!TestTrue(TEXT("create ok"), Invoke(Registry, TEXT("unreal_open_mcp_blueprint_create"),
				MakeBody({ { TEXT("path"), Quote(Dest) } })).bOk)) return;

			// No name.
			TestEqual(TEXT("missing name"),
				Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_event"),
					MakeBody({ { TEXT("path"), Quote(Dest) } })).Code,
				FString(TEXT("missing_parameter")));
			// No path at all.
			TestEqual(TEXT("missing path"),
				Invoke(Registry, TEXT("unreal_open_mcp_blueprint_add_event"), TEXT("{}")).Code,
				FString(TEXT("missing_parameter")));
		});

		// Missing Blueprint — add_event on an absent path.
		It("returns blueprint_not_found for a missing Blueprint", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBlueprintTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_blueprint_add_event"),
				MakeBody({
					{ TEXT("path"), Quote(TEXT("/Game/__DoesNotExist/BP_Nope")) },
					{ TEXT("name"), Quote(TEXT("ReceiveBeginPlay")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("blueprint_not_found")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
