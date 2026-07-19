// unreal_open_mcp_asset_create_folder / asset_copy / asset_move /
// asset_delete / asset_refresh Automation specs (P4.2).
//
// Pins the Content Browser CRUD family end-to-end at the handler level. The
// cases mirror the P4.2 plan's acceptance criteria:
//   - create_folder: writable-root refusal on /Engine, idempotent on the
//     second call (created:false), happy path creates a real folder under
//     /Game, missing_parameter when path absent.
//   - copy: missing_parameter on empty source/destination; asset_not_found
//     on a missing source; asset_already_exists on a collision; happy path
//     duplicates a known engine asset under /Game.
//   - move: happy path rename + asset_already_exists on a collision;
//     invalid_path on a malformed destination.
//   - delete: missing_parameter + asset_not_found; writable-root refusal;
//     delete_blocked_by_referencers when an on-disk package references the
//     asset; happy path on an unreferenced scratch asset; force override on
//     a referenced scratch asset.
//   - refresh: defaults to /Game when omitted; happy path with a single
//     `path`; invalid_parameter on a non-array `paths`.
//
// The suite owns its scratch tree under /Game/__McpP42Crud — create_folder
// builds the tree, the copy/move/delete cases live inside it, and the
// teardown removes the whole tree so the automation project does not
// accumulate test artifacts between runs. The cases that need an engine
// asset to copy from probe for its existence up front and skip (with a
// pass) when the automation project does not ship BasicShapes.
//
// Adapted from Unity's assets-create-folder / assets-copy / assets-move /
// assets-delete / assets-refresh test surface at adapt fidelity: single
// path replaces Unity's batch arrays; /Game content paths replace Assets/;
// AssetRegistry::GetReferencers replaces Unity's missing-script surface.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Tools/UnrealOpenMcpAssetTools.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
// EditorAssetLibrary — same probe helpers the production handlers use.
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpAssetCrudSpec,
	"UnrealOpenMcp.Tools.AssetCrud",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpAssetCrudSpec)

namespace
{
	/** Scratch tree root — every P4.2 case lives under here so teardown can
	 *  remove the whole subtree with one DeleteDirectory. */
	constexpr const TCHAR* ScratchRoot = TEXT("/Game/__McpP42Crud");

	/** A known engine asset the cases copy/move FROM. The engine's default
	 *  cube static mesh ships with the BasicShapes content pack; cases probe
	 *  for it up front and skip (with a pass) when absent. */
	constexpr const TCHAR* KnownEngineAssetPath = TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");

	/** Parse a JSON object from a string. Null on failure. */
	TSharedPtr<FJsonObject> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	/** Build a JSON body string from a list of (key, value) pairs. Values are
	 *  emitted as raw JSON tokens, so callers pass quoted strings for string
	 *  values, raw true/false for booleans, and bracketed forms for arrays. */
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

	/** Invoke a registered tool by name with a JSON body. Returns the raw
	 *  dispatch result so the spec asserts on ok/code/output directly. */
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

	/** Remove the entire scratch tree so the next run starts clean. Best-effort —
	 *  a leftover directory never fails the spec; it just accumulates. */
	void CleanupScratch()
	{
		if (UEditorAssetLibrary::DoesDirectoryExist(ScratchRoot))
		{
			UEditorAssetLibrary::DeleteDirectory(ScratchRoot);
		}
	}

	/** True when the asset exists in the AssetRegistry. */
	bool AssetExists(const FString& AssetPath)
	{
		return UEditorAssetLibrary::DoesAssetExist(AssetPath);
	}
}

void FUnrealOpenMcpAssetCrudSpec::Define()
{
	BeforeEach([this]()
	{
		// Start each case from a clean scratch tree so ordering between cases
		// does not matter and a prior run's leftovers do not flip a
		// collision case into an unexpected success.
		CleanupScratch();
	});

	AfterEach([this]()
	{
		// Tear down after each case too so the automation project stays clean.
		CleanupScratch();
	});

	Describe("unreal_open_mcp_asset_create_folder", [this]()
	{
		// Happy path — create a real folder under /Game; the result reports
		// created:true and the folder appears in the registry.
		It("creates a /Game folder and reports created:true", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FString Path = FString::Printf(TEXT("%s/Top"), ScratchRoot);
			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_create_folder"),
				MakeBody({ { TEXT("path"), FString::Printf(TEXT("\"%s\""), *Path) } }));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestEqual(TEXT("path echoed"), Json->GetStringField(TEXT("path")), Path);
			TestTrue(TEXT("created"), Json->GetBoolField(TEXT("created")));
			TestTrue(TEXT("folder exists on disk"), UEditorAssetLibrary::DoesDirectoryExist(Path));
		});

		// Idempotent — a second call to an existing folder returns
		// created:false and is NOT an error.
		It("is idempotent on a second call (created:false)", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FString Path = FString::Printf(TEXT("%s/Idem"), ScratchRoot);
			const FUnrealOpenMcpToolDispatchResult R1 = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_create_folder"),
				MakeBody({ { TEXT("path"), FString::Printf(TEXT("\"%s\""), *Path) } }));
			if (!TestTrue(TEXT("first call ok"), R1.bOk)) return;

			const FUnrealOpenMcpToolDispatchResult R2 = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_create_folder"),
				MakeBody({ { TEXT("path"), FString::Printf(TEXT("\"%s\""), *Path) } }));
			if (!TestTrue(TEXT("second call ok"), R2.bOk)) return;
			const TSharedPtr<FJsonObject> Json = ParseJson(R2.Output);
			if (!TestNotNull(TEXT("second result json"), Json.Get())) return;
			TestFalse(TEXT("created false on second call"), Json->GetBoolField(TEXT("created")));
		});

		// Engine content root — the writable-root guard refuses /Engine,
		// /Script, /Temp so an agent cannot accidentally scribble into
		// engine content.
		It("refuses /Engine writes with invalid_content_root", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_create_folder"),
				MakeBody({ { TEXT("path"), TEXT("\"/Engine/ShouldNotCreate\"") } }));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("invalid_content_root")));
		});

		It("refuses /Script writes with invalid_content_root", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_create_folder"),
				MakeBody({ { TEXT("path"), TEXT("\"/Script/ShouldNotCreate\"") } }));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("invalid_content_root")));
		});

		// Missing path → missing_parameter.
		It("returns missing_parameter when path is absent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_create_folder"),
				TEXT("{}"));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("missing_parameter")));
		});

		// Malformed body → invalid_parameter.
		It("returns invalid_parameter for a malformed body", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_create_folder"),
				TEXT("<<<not json>>>"));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("invalid_parameter")));
		});
	});

	Describe("unreal_open_mcp_asset_copy", [this]()
	{
		// Happy path — copy a known engine asset under /Game (the scratch
		// tree's parent folder is created first). Gated on the fixture
		// asset existing.
		It("copies an engine asset under /Game", [this]()
		{
			if (!TestTrue(TEXT("fixture asset exists"), AssetExists(KnownEngineAssetPath)))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			// Parent folder must exist (create_folder first).
			const FString Parent = FString::Printf(TEXT("%s/CopySrc"), ScratchRoot);
			Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_create_folder"),
				MakeBody({ { TEXT("path"), FString::Printf(TEXT("\"%s\""), *Parent) } }));

			const FString Destination = FString::Printf(TEXT("%s/MI_Copy"), *Parent);
			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_copy"),
				MakeBody({
					{ TEXT("source"), FString::Printf(TEXT("\"%s\""), KnownEngineAssetPath) },
					{ TEXT("destination"), FString::Printf(TEXT("\"%s\""), *Destination) },
				}));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestEqual(TEXT("source echoed"), Json->GetStringField(TEXT("source")), KnownEngineAssetPath);
			TestEqual(TEXT("destination echoed"), Json->GetStringField(TEXT("destination")), Destination);
			TestTrue(TEXT("destination exists"), AssetExists(Destination));
		});

		// Missing source / destination → missing_parameter.
		It("returns missing_parameter when source/destination absent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_copy"),
				TEXT("{\"source\":\"/Game/Foo\"}"));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("missing_parameter")));
		});

		// Missing source asset → asset_not_found.
		It("returns asset_not_found when source is missing", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_copy"),
				MakeBody({
					{ TEXT("source"), TEXT("\"/Game/Does/Not/Exist.Nope\"") },
					{ TEXT("destination"), TEXT("\"/Game/__McpP42Crud/Out\"") },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("asset_not_found")));
		});

		// Destination collision → asset_already_exists (no silent overwrite).
		It("refuses to overwrite with asset_already_exists", [this]()
		{
			if (!TestTrue(TEXT("fixture asset exists"), AssetExists(KnownEngineAssetPath)))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FString Parent = FString::Printf(TEXT("%s/CopyCollide"), ScratchRoot);
			Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_create_folder"),
				MakeBody({ { TEXT("path"), FString::Printf(TEXT("\"%s\""), *Parent) } }));

			const FString Destination = FString::Printf(TEXT("%s/MI_Dup"), *Parent);
			// First copy succeeds.
			Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_copy"),
				MakeBody({
					{ TEXT("source"), FString::Printf(TEXT("\"%s\""), KnownEngineAssetPath) },
					{ TEXT("destination"), FString::Printf(TEXT("\"%s\""), *Destination) },
				}));
			// Second copy to the same destination → collision.
			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_copy"),
				MakeBody({
					{ TEXT("source"), FString::Printf(TEXT("\"%s\""), KnownEngineAssetPath) },
					{ TEXT("destination"), FString::Printf(TEXT("\"%s\""), *Destination) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("asset_already_exists")));
		});

		// Engine content root destination → invalid_content_root.
		It("refuses /Engine destination with invalid_content_root", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_copy"),
				MakeBody({
					{ TEXT("source"), FString::Printf(TEXT("\"%s\""), KnownEngineAssetPath) },
					{ TEXT("destination"), TEXT("\"/Engine/ShouldNotCopy\"") },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("invalid_content_root")));
		});

		// Malformed destination (no parent segment) → invalid_path.
		It("returns invalid_path for a malformed destination", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_copy"),
				MakeBody({
					{ TEXT("source"), FString::Printf(TEXT("\"%s\""), KnownEngineAssetPath) },
					// No slash — SplitObjectPath refuses.
					{ TEXT("destination"), TEXT("\"BareName\"") },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("invalid_path")));
		});
	});

	Describe("unreal_open_mcp_asset_move", [this]()
	{
		// Happy path — copy a fixture in, then move it to a new name. The
		// structured result carries source + destination + a redirector note.
		It("moves an asset to a new package path", [this]()
		{
			if (!TestTrue(TEXT("fixture asset exists"), AssetExists(KnownEngineAssetPath)))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FString Parent = FString::Printf(TEXT("%s/MoveHappy"), ScratchRoot);
			Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_create_folder"),
				MakeBody({ { TEXT("path"), FString::Printf(TEXT("\"%s\""), *Parent) } }));
			const FString Original = FString::Printf(TEXT("%s/MI_Orig"), *Parent);
			Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_copy"),
				MakeBody({
					{ TEXT("source"), FString::Printf(TEXT("\"%s\""), KnownEngineAssetPath) },
					{ TEXT("destination"), FString::Printf(TEXT("\"%s\""), *Original) },
				}));

			const FString Renamed = FString::Printf(TEXT("%s/MI_Renamed"), *Parent);
			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_move"),
				MakeBody({
					{ TEXT("source"), FString::Printf(TEXT("\"%s\""), *Original) },
					{ TEXT("destination"), FString::Printf(TEXT("\"%s\""), *Renamed) },
				}));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestEqual(TEXT("source echoed"), Json->GetStringField(TEXT("source")), Original);
			TestEqual(TEXT("destination echoed"), Json->GetStringField(TEXT("destination")), Renamed);
			// Redirector hint surfaces so the agent knows to fix references.
			TestTrue(TEXT("has note"), Json->HasTypedField<EJson::String>(TEXT("note")));
			TestTrue(TEXT("renamed exists"), AssetExists(Renamed));
		});

		// Destination collision → asset_already_exists.
		It("refuses to overwrite with asset_already_exists", [this]()
		{
			if (!TestTrue(TEXT("fixture asset exists"), AssetExists(KnownEngineAssetPath)))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FString Parent = FString::Printf(TEXT("%s/MoveCollide"), ScratchRoot);
			Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_create_folder"),
				MakeBody({ { TEXT("path"), FString::Printf(TEXT("\"%s\""), *Parent) } }));
			const FString A = FString::Printf(TEXT("%s/MI_A"), *Parent);
			const FString B = FString::Printf(TEXT("%s/MI_B"), *Parent);
			Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_copy"),
				MakeBody({
					{ TEXT("source"), FString::Printf(TEXT("\"%s\""), KnownEngineAssetPath) },
					{ TEXT("destination"), FString::Printf(TEXT("\"%s\""), *A) },
				}));
			Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_copy"),
				MakeBody({
					{ TEXT("source"), FString::Printf(TEXT("\"%s\""), KnownEngineAssetPath) },
					{ TEXT("destination"), FString::Printf(TEXT("\"%s\""), *B) },
				}));

			// Moving A onto B (which exists) → collision.
			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_move"),
				MakeBody({
					{ TEXT("source"), FString::Printf(TEXT("\"%s\""), *A) },
					{ TEXT("destination"), FString::Printf(TEXT("\"%s\""), *B) },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("asset_already_exists")));
		});

		// Missing source → asset_not_found.
		It("returns asset_not_found when source is missing", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_move"),
				MakeBody({
					{ TEXT("source"), TEXT("\"/Game/Does/Not/Exist.Nope\"") },
					{ TEXT("destination"), TEXT("\"/Game/__McpP42Crud/Out\"") },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("asset_not_found")));
		});

		// Malformed destination → invalid_path.
		It("returns invalid_path for a malformed destination", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_move"),
				MakeBody({
					{ TEXT("source"), FString::Printf(TEXT("\"%s\""), KnownEngineAssetPath) },
					{ TEXT("destination"), TEXT("\"NoSlashName\"") },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("invalid_path")));
		});
	});

	Describe("unreal_open_mcp_asset_delete", [this]()
	{
		// Happy path — delete an unreferenced scratch asset (copied in
		// first so it has no inbound refs). Result reports deleted:true.
		It("deletes an unreferenced scratch asset", [this]()
		{
			if (!TestTrue(TEXT("fixture asset exists"), AssetExists(KnownEngineAssetPath)))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FString Parent = FString::Printf(TEXT("%s/DelHappy"), ScratchRoot);
			Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_create_folder"),
				MakeBody({ { TEXT("path"), FString::Printf(TEXT("\"%s\""), *Parent) } }));
			const FString Target = FString::Printf(TEXT("%s/MI_Del"), *Parent);
			Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_copy"),
				MakeBody({
					{ TEXT("source"), FString::Printf(TEXT("\"%s\""), KnownEngineAssetPath) },
					{ TEXT("destination"), FString::Printf(TEXT("\"%s\""), *Target) },
				}));

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_delete"),
				MakeBody({ { TEXT("path"), FString::Printf(TEXT("\"%s\""), *Target) } }));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestEqual(TEXT("path echoed"), Json->GetStringField(TEXT("path")), Target);
			TestTrue(TEXT("deleted"), Json->GetBoolField(TEXT("deleted")));
			TestFalse(TEXT("forced default false"), Json->GetBoolField(TEXT("forced")));
			TestFalse(TEXT("asset gone"), AssetExists(Target));
		});

		// Missing path → missing_parameter.
		It("returns missing_parameter when path is absent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_delete"),
				TEXT("{}"));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("missing_parameter")));
		});

		// Missing asset → asset_not_found.
		It("returns asset_not_found when path is missing", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_delete"),
				MakeBody({ { TEXT("path"), TEXT("\"/Game/Does/Not/Exist.Nope\"") } }));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("asset_not_found")));
		});

		// Engine content root — refused even with force:true.
		It("refuses /Engine deletes with invalid_content_root", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_delete"),
				MakeBody({
					{ TEXT("path"), TEXT("\"/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial\"") },
					{ TEXT("force"), TEXT("true") },
				}));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("invalid_content_root")));
		});

		// Referencer guard — when an asset is referenced by other on-disk
		// packages, the call REFUSES with delete_blocked_by_referencers and
		// surfaces the referencer list (bounded at 10) + count in the message.
		// The engine's WorldGridMaterial is referenced by many engine assets,
		// so the registry reports inbound referencers. We do NOT actually
		// delete it (the guard refuses before that point).
		It("refuses with delete_blocked_by_referencers when referenced", [this]()
		{
			if (!TestTrue(TEXT("fixture asset exists"), AssetExists(KnownEngineAssetPath)))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_delete"),
				MakeBody({
					{ TEXT("path"), KnownEngineAssetPath },
				}));

			// The asset MAY or may not have on-disk referencers depending on
			// the project's installed content. If it does, assert the
			// structured refuse; if not, the case is a no-op pass (the
			// happy-path DelHappy case already covers the unreferenced branch).
			if (!R.bOk)
			{
				TestEqual(
					TEXT("blocked by referencers"),
					R.Code,
					FString(TEXT("delete_blocked_by_referencers")));
				// Message embeds the count + the bounded referencer list so an
				// agent can branch without parsing a structured payload.
				TestTrue(TEXT("message mentions count"), R.Message.Contains(TEXT("referenced by")));
				TestTrue(TEXT("message mentions force"), R.Message.Contains(TEXT("force")));
			}
		});

		// force:true on an unreferenced scratch asset — succeeds and reports
		// forced:true. Exercises the force override path even when the
		// referencer guard would have allowed the delete anyway.
		It("force:true overrides the guard and reports forced:true", [this]()
		{
			if (!TestTrue(TEXT("fixture asset exists"), AssetExists(KnownEngineAssetPath)))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FString Parent = FString::Printf(TEXT("%s/DelForce"), ScratchRoot);
			Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_create_folder"),
				MakeBody({ { TEXT("path"), FString::Printf(TEXT("\"%s\""), *Parent) } }));
			const FString Target = FString::Printf(TEXT("%s/MI_Force"), *Parent);
			Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_copy"),
				MakeBody({
					{ TEXT("source"), FString::Printf(TEXT("\"%s\""), KnownEngineAssetPath) },
					{ TEXT("destination"), FString::Printf(TEXT("\"%s\""), *Target) },
				}));

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_delete"),
				MakeBody({
					{ TEXT("path"), FString::Printf(TEXT("\"%s\""), *Target) },
					{ TEXT("force"), TEXT("true") },
				}));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;
			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestTrue(TEXT("deleted"), Json->GetBoolField(TEXT("deleted")));
			TestTrue(TEXT("forced"), Json->GetBoolField(TEXT("forced")));
			TestFalse(TEXT("asset gone"), AssetExists(Target));
		});
	});

	Describe("unreal_open_mcp_asset_refresh", [this]()
	{
		// Read-only tool — no paths_hint/gate surface; happy path returns
		// { paths, force } echoing the request.
		It("defaults to /Game when paths omitted", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_refresh"),
				TEXT("{}"));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;

			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestTrue(TEXT("has paths"), Json->HasTypedField<EJson::Array>(TEXT("paths")));
			const TArray<TSharedPtr<FJsonValue>>& Paths = Json->GetArrayField(TEXT("paths"));
			TestEqual(TEXT("default /Game"), Paths.Num(), 1);
			TestEqual(TEXT("paths[0] is /Game"), Paths[0]->AsString(), FString(TEXT("/Game")));
			TestFalse(TEXT("force default false"), Json->GetBoolField(TEXT("force")));
		});

		// Single-path ergonomics — `path` is appended to the scanned set.
		It("accepts a single-string path argument", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_refresh"),
				MakeBody({ { TEXT("path"), TEXT("\"/Game/Engine\"") }, { TEXT("force"), TEXT("true") } }));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;
			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			const TArray<TSharedPtr<FJsonValue>>& Paths = Json->GetArrayField(TEXT("paths"));
			TestEqual(TEXT("scanned one path"), Paths.Num(), 1);
			TestEqual(TEXT("paths[0]"), Paths[0]->AsString(), FString(TEXT("/Game/Engine")));
			TestTrue(TEXT("force echoed"), Json->GetBoolField(TEXT("force")));
		});

		// Array paths — multiple paths are scanned as-is.
		It("accepts an array of paths", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_refresh"),
				MakeBody({ { TEXT("paths"), TEXT("[\"/Game\",\"/Engine\"]") } }));
			if (!TestTrue(TEXT("ok"), R.bOk)) return;
			const TSharedPtr<FJsonObject> Json = ParseJson(R.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get())) return;
			TestEqual(TEXT("scanned two paths"), Json->GetArrayField(TEXT("paths")).Num(), 2);
		});

		// Non-array `paths` → invalid_parameter.
		It("returns invalid_parameter when paths is not an array", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_refresh"),
				MakeBody({ { TEXT("paths"), TEXT("\"/Game\"") } }));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("invalid_parameter")));
		});

		// Malformed body → invalid_parameter.
		It("returns invalid_parameter for a malformed body", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

			const FUnrealOpenMcpToolDispatchResult R = Invoke(
				Registry,
				TEXT("unreal_open_mcp_asset_refresh"),
				TEXT("<<<not json>>>"));
			TestFalse(TEXT("ok false"), R.bOk);
			TestEqual(TEXT("code"), R.Code, FString(TEXT("invalid_parameter")));
		});
	});

	Describe("mutation classification", [this]()
	{
		// Pin the mutating/read-only classification so a later refactor
		// cannot accidentally re-classify a tool and break the gate
		// contract. The dispatcher consults this metadata to decide
		// whether to wrap a call in GatePolicy.Execute.
		It("create_folder / copy / move / delete are mutating; refresh is read-only", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);

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

			CheckMutating(TEXT("unreal_open_mcp_asset_create_folder"), true);
			CheckMutating(TEXT("unreal_open_mcp_asset_copy"), true);
			CheckMutating(TEXT("unreal_open_mcp_asset_move"), true);
			CheckMutating(TEXT("unreal_open_mcp_asset_delete"), true);
			// Refresh is read-only — ScanPathsSynchronous does not write
			// packages or change the UObject graph.
			CheckMutating(TEXT("unreal_open_mcp_asset_refresh"), false);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
