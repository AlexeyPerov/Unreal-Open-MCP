// unreal_open_mcp_material_create / material_modify / material_get_data
// Automation specs (P4.3).
//
// Pins the material tool family end-to-end at the handler level. The cases
// mirror the P4.3 plan's acceptance criteria + test list:
//   - create: happy path (MIC from a valid parent material interface under
//     /Game, result echoes path + parent); parent missing (parent_not_found);
//     writable-root refusal on /Engine (invalid_content_root); collision on an
//     existing destination (asset_already_exists); missing_parameter.
//   - modify: empty no-op rejected (nothing_to_modify); wrong type — a base
//     UMaterial is not a UMaterialInstanceConstant (not_a_material_instance);
//     asset_not_found; unknown parameter names land in `failed` and, when
//     nothing applies, the call reports nothing_to_modify (no dirty); positive
//     parameter apply when the parent exposes a scalar/vector parameter.
//   - get-data: round-trip after create (isInstance:true, parent set,
//     scalars/vectors/textures objects present); base material read
//     (isInstance:false); asset_not_found; missing_parameter.
//   - mutation classification: create/modify mutating, get-data read-only.
//
// The suite owns its scratch tree under /Game/__McpP43Material — teardown
// removes the whole subtree so the automation project does not accumulate test
// artifacts between runs. Cases that need an engine parent material to create
// FROM probe for its existence up front and skip (with a pass) when the
// automation environment does not ship it.
//
// Adapted from Unity's material-create / material-set-property /
// material-get-properties test surface at adapt fidelity: MIC-from-parent
// replaces Unity's shader-name create; batch scalars/vectors/textures maps
// replace Unity's single-property set; /Game content paths replace Assets/.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Tools/UnrealOpenMcpMaterialTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "EditorAssetLibrary.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpMaterialToolsSpec,
	"UnrealOpenMcp.Tools.MaterialTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpMaterialToolsSpec)

namespace
{
	/** Scratch tree root — every P4.3 case lives under here so teardown can
	 *  remove the whole subtree with one DeleteDirectory. */
	constexpr const TCHAR* MaterialScratchRoot = TEXT("/Game/__McpP43Material");

	/** A known engine base material the create cases parent FROM. Ships with the
	 *  engine (not the project), so it is reliably present in the editor test
	 *  runner; cases still probe for it and skip (with a pass) when absent. */
	constexpr const TCHAR* KnownParentMaterialPath = TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");

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

	/** Remove the entire scratch tree so the next run starts clean. Best-effort. */
	void CleanupScratch()
	{
		if (UEditorAssetLibrary::DoesDirectoryExist(MaterialScratchRoot))
		{
			UEditorAssetLibrary::DeleteDirectory(MaterialScratchRoot);
		}
	}

	/** Quote a string as a JSON token. */
	FString Quote(const FString& S)
	{
		return FString::Printf(TEXT("\"%s\""), *S);
	}
}

void FUnrealOpenMcpMaterialToolsSpec::Define()
{
	BeforeEach([this]()
	{
		CleanupScratch();
	});

	AfterEach([this]()
	{
		CleanupScratch();
	});

	Describe("unreal_open_mcp_material_create", [this]()
	{
		// Happy path — create a MIC from a valid parent material interface under
		// /Game; the result echoes path + parent and the asset exists.
		It("creates a MIC from a valid parent and reports path + parent", [this]()
		{
			if (!UEditorAssetLibrary::DoesAssetExist(KnownParentMaterialPath))
			{
				// Environment does not ship the parent material — skip (pass).
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpMaterialTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/MI_Created"), MaterialScratchRoot);
			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_create"),
				MakeBody({
					{ TEXT("parent"), Quote(KnownParentMaterialPath) },
					{ TEXT("destination"), Quote(Dest) },
				}));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestTrue(TEXT("path contains dest"), Json->GetStringField(TEXT("path")).Contains(TEXT("MI_Created")));
			TestFalse(TEXT("parent echoed"), Json->GetStringField(TEXT("parent")).IsEmpty());
			TestTrue(TEXT("asset exists on disk"), UEditorAssetLibrary::DoesAssetExist(Dest));
		});

		// Parent missing — a non-existent parent path returns parent_not_found
		// (the DoesAssetExist pre-check avoids a logged engine LoadAsset error).
		It("returns parent_not_found for a missing parent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpMaterialTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/MI_NoParent"), MaterialScratchRoot);
			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_create"),
				MakeBody({
					{ TEXT("parent"), Quote(TEXT("/Game/__DoesNotExist/M_Nope")) },
					{ TEXT("destination"), Quote(Dest) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("parent_not_found")));
		});

		// Writable-root guard — refuse creating under /Engine.
		It("refuses an /Engine destination with invalid_content_root", [this]()
		{
			if (!UEditorAssetLibrary::DoesAssetExist(KnownParentMaterialPath))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpMaterialTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_create"),
				MakeBody({
					{ TEXT("parent"), Quote(KnownParentMaterialPath) },
					{ TEXT("destination"), Quote(TEXT("/Engine/__Mcp/MI_ShouldNotCreate")) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("invalid_content_root")));
		});

		// Collision — creating over an existing asset is refused.
		It("refuses an existing destination with asset_already_exists", [this]()
		{
			if (!UEditorAssetLibrary::DoesAssetExist(KnownParentMaterialPath))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpMaterialTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/MI_Dup"), MaterialScratchRoot);
			const FUnrealOpenMcpToolDispatchResult R1 = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_create"),
				MakeBody({
					{ TEXT("parent"), Quote(KnownParentMaterialPath) },
					{ TEXT("destination"), Quote(Dest) },
				}));
			if (!TestTrue(TEXT("first create ok"), R1.bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R2 = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_create"),
				MakeBody({
					{ TEXT("parent"), Quote(KnownParentMaterialPath) },
					{ TEXT("destination"), Quote(Dest) },
				}));
			TestFalse(TEXT("second create fails"), R2.bOk);
			TestEqual(TEXT("code"), R2.Code, FString(TEXT("asset_already_exists")));
		});

		// Missing args.
		It("returns missing_parameter when parent/destination absent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpMaterialTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_create"),
				TEXT("{}"));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("missing_parameter")));
		});
	});

	Describe("unreal_open_mcp_material_modify", [this]()
	{
		// Empty no-op — no scalars/vectors/textures → nothing_to_modify.
		It("rejects an empty modify with nothing_to_modify", [this]()
		{
			if (!UEditorAssetLibrary::DoesAssetExist(KnownParentMaterialPath))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpMaterialTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/MI_Empty"), MaterialScratchRoot);
			const FUnrealOpenMcpToolDispatchResult Create = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_create"),
				MakeBody({
					{ TEXT("parent"), Quote(KnownParentMaterialPath) },
					{ TEXT("destination"), Quote(Dest) },
				}));
			if (!TestTrue(TEXT("create ok"), Create.bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_modify"),
				MakeBody({ { TEXT("path"), Quote(Dest) } }));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("nothing_to_modify")));
		});

		// Wrong type — a base UMaterial is not a UMaterialInstanceConstant.
		It("returns not_a_material_instance for a base material", [this]()
		{
			if (!UEditorAssetLibrary::DoesAssetExist(KnownParentMaterialPath))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpMaterialTools::Register(Registry);

			// Copy the engine base material into /Game so the writable-root
			// guard passes and the type check is what fails.
			const FString Dest = FString::Printf(TEXT("%s/M_BaseCopy"), MaterialScratchRoot);
			if (!UEditorAssetLibrary::DuplicateAsset(KnownParentMaterialPath, Dest))
			{
				// Could not stage the base material — skip (pass).
				return;
			}

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_modify"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("scalars"), TEXT("{\"AnyParam\":1.0}") },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("not_a_material_instance")));
		});

		// Missing asset.
		It("returns asset_not_found for a missing path", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpMaterialTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_modify"),
				MakeBody({
					{ TEXT("path"), Quote(TEXT("/Game/__DoesNotExist/MI_Nope")) },
					{ TEXT("scalars"), TEXT("{\"X\":1.0}") },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("asset_not_found")));
		});

		// Unknown parameter names → land in `failed`; nothing applies → the call
		// reports nothing_to_modify (and leaves the package untouched).
		It("classifies unknown parameter names as failed (nothing_to_modify)", [this]()
		{
			if (!UEditorAssetLibrary::DoesAssetExist(KnownParentMaterialPath))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpMaterialTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/MI_Unknown"), MaterialScratchRoot);
			const FUnrealOpenMcpToolDispatchResult Create = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_create"),
				MakeBody({
					{ TEXT("parent"), Quote(KnownParentMaterialPath) },
					{ TEXT("destination"), Quote(Dest) },
				}));
			if (!TestTrue(TEXT("create ok"), Create.bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_modify"),
				MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("scalars"), TEXT("{\"__DefinitelyNotARealParam__\":1.0}") },
				}));
			// No known parameter matched → nothing applied → nothing_to_modify.
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("nothing_to_modify")));
		});

		// Positive apply — when the parent exposes a scalar or vector parameter,
		// applying it lands in the grouped `applied` block. Adaptive: skip (pass)
		// when the parent material has no scalar/vector parameters to set.
		It("applies a real scalar/vector parameter when the parent exposes one", [this]()
		{
			if (!UEditorAssetLibrary::DoesAssetExist(KnownParentMaterialPath))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpMaterialTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/MI_Apply"), MaterialScratchRoot);
			const FUnrealOpenMcpToolDispatchResult Create = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_create"),
				MakeBody({
					{ TEXT("parent"), Quote(KnownParentMaterialPath) },
					{ TEXT("destination"), Quote(Dest) },
				}));
			if (!TestTrue(TEXT("create ok"), Create.bOk)) return;

			// Read the parameter inventory to pick a real parameter name.
			const FUnrealOpenMcpToolDispatchResult GetData = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_get_data"),
				MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("get-data ok"), GetData.bOk)) return;
			const TSharedPtr<FJsonObject> DataJson = ParseJson(GetData.Output);
			if (!TestNotNull(TEXT("get-data json"), DataJson.Get())) return;

			const TSharedPtr<FJsonObject>* ScalarsObj = nullptr;
			const TSharedPtr<FJsonObject>* VectorsObj = nullptr;
			DataJson->TryGetObjectField(TEXT("scalars"), ScalarsObj);
			DataJson->TryGetObjectField(TEXT("vectors"), VectorsObj);

			FString Body;
			FString ExpectedGroup;
			FString ExpectedName;
			if (ScalarsObj && (*ScalarsObj)->Values.Num() > 0)
			{
				ExpectedName = (*ScalarsObj)->Values.CreateConstIterator()->Key;
				ExpectedGroup = TEXT("scalars");
				Body = MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("scalars"), FString::Printf(TEXT("{\"%s\":0.5}"), *ExpectedName) },
				});
			}
			else if (VectorsObj && (*VectorsObj)->Values.Num() > 0)
			{
				ExpectedName = (*VectorsObj)->Values.CreateConstIterator()->Key;
				ExpectedGroup = TEXT("vectors");
				Body = MakeBody({
					{ TEXT("path"), Quote(Dest) },
					{ TEXT("vectors"), FString::Printf(TEXT("{\"%s\":{\"r\":1.0}}"), *ExpectedName) },
				});
			}
			else
			{
				// Parent exposes no scalar/vector parameters — skip (pass).
				return;
			}

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry, TEXT("unreal_open_mcp_material_modify"), Body);
			if (!TestTrue(TEXT("modify ok"), R.bOk)) return;
			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("modify json"), Json.Get())) return;

			const TSharedPtr<FJsonObject>* AppliedObj = nullptr;
			if (!TestTrue(TEXT("applied object present"), Json->TryGetObjectField(TEXT("applied"), AppliedObj))) return;
			const TArray<TSharedPtr<FJsonValue>>* AppliedGroup = nullptr;
			if (!TestTrue(TEXT("applied group present"), (*AppliedObj)->TryGetArrayField(ExpectedGroup, AppliedGroup))) return;
			TestEqual(TEXT("one parameter applied"), AppliedGroup->Num(), 1);
		});
	});

	Describe("unreal_open_mcp_material_get_data", [this]()
	{
		// Round-trip after create — isInstance:true, parent set, parameter
		// objects present.
		It("reads a created MIC (isInstance:true, parent, parameter objects)", [this]()
		{
			if (!UEditorAssetLibrary::DoesAssetExist(KnownParentMaterialPath))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpMaterialTools::Register(Registry);

			const FString Dest = FString::Printf(TEXT("%s/MI_Read"), MaterialScratchRoot);
			const FUnrealOpenMcpToolDispatchResult Create = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_create"),
				MakeBody({
					{ TEXT("parent"), Quote(KnownParentMaterialPath) },
					{ TEXT("destination"), Quote(Dest) },
				}));
			if (!TestTrue(TEXT("create ok"), Create.bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_get_data"),
				MakeBody({ { TEXT("path"), Quote(Dest) } }));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;
			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestTrue(TEXT("isInstance"), Json->GetBoolField(TEXT("isInstance")));
			TestFalse(TEXT("parent set"), Json->GetStringField(TEXT("parent")).IsEmpty());
			TestTrue(TEXT("scalars object"), Json->HasTypedField<EJson::Object>(TEXT("scalars")));
			TestTrue(TEXT("vectors object"), Json->HasTypedField<EJson::Object>(TEXT("vectors")));
			TestTrue(TEXT("textures object"), Json->HasTypedField<EJson::Object>(TEXT("textures")));
		});

		// Base material read — isInstance:false (defaults reported, not null).
		It("reads a base material (isInstance:false)", [this]()
		{
			if (!UEditorAssetLibrary::DoesAssetExist(KnownParentMaterialPath))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpMaterialTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_get_data"),
				MakeBody({ { TEXT("path"), Quote(KnownParentMaterialPath) } }));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;
			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestFalse(TEXT("isInstance false"), Json->GetBoolField(TEXT("isInstance")));
		});

		// Missing asset.
		It("returns asset_not_found for a missing path", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpMaterialTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_get_data"),
				MakeBody({ { TEXT("path"), Quote(TEXT("/Game/__DoesNotExist/M_Nope")) } }));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("asset_not_found")));
		});

		// Missing arg.
		It("returns missing_parameter when path absent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpMaterialTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_material_get_data"),
				TEXT("{}"));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("missing_parameter")));
		});
	});

	Describe("mutation classification", [this]()
	{
		// Pin the mutating/read-only classification so a later refactor cannot
		// accidentally re-classify a tool and break the gate contract.
		It("create / modify are mutating; get_data is read-only", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpMaterialTools::Register(Registry);

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

			CheckMutating(TEXT("unreal_open_mcp_material_create"), true);
			CheckMutating(TEXT("unreal_open_mcp_material_modify"), true);
			CheckMutating(TEXT("unreal_open_mcp_material_get_data"), false);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
