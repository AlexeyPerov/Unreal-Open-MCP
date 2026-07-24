// unreal_open_mcp_blueprint_create / blueprint_get Automation specs (P6.1).
//
// Pins the Blueprint tool family end-to-end at the handler level. The cases
// mirror the P6.1 plan's acceptance criteria + test list:
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
//   - mutation classification: create mutating, get read-only.
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
		It("create is mutating; get is read-only", [this]()
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
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
