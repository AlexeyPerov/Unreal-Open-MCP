// Level-tool family — see header for the open/save/list/set-current/unload
// contract and the dirty-guard semantics. This file owns the five level
// lifecycle handlers plus the shared `ToLevelData` serializer and the
// path-normalisation helpers (`ResolveLevelFilename`, `ResolveSaveTarget`).
//
// Arg parsing + output mirror the actor family: each handler parses the raw
// POST body into an FJsonObject and emits a pre-serialized JSON string handed
// to FUnrealOpenMcpToolDispatchResult::Output. The registry/dispatch spine
// stays raw-body — only the handler layer parses.
//
// Behavior reference (read-only): Unreal-MCP's level handlers
// (UnrealMcpLevelTools.cpp). The path-normalisation + content-aware existence
// probe, the save-in-place transient guard, the short-name-vs-package-path
// disambiguation, and the discardedDirtyLevels note were studied for correct
// Unreal editor API usage and adapted to this port's Ok/Fail result shape.
#include "Tools/UnrealOpenMcpLevelTools.h"

#include "Bridge/UnrealOpenMcpLevelDirtyGuard.h"
#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "UnrealOpenMcpLog.h"

// FileHelpers.h owns UEditorLoadingAndSavingUtils — the UnrealEd surface for
// LoadMap / SaveMap / SaveCurrentLevel / GetDirtyMapPackages. Lives in UnrealEd
// (already a Private dependency of UnrealOpenMcpEditor).
#include "Editor/FileHelpers.h"
// EditorLevelUtils.h owns UEditorLevelUtils::MakeLevelCurrent /
// RemoveLevelFromWorld — the editor path for level_set_current and
// level_unload_sublevel. Lives in UnrealEd.
#include "Editor/EditorLevelUtils.h"
#include "Editor.h"                 // GEditor
#include "Engine/World.h"
#include "Engine/Level.h"
// LevelStreaming.h owns ULevelStreaming — the streaming sublevel surface
// enumerated by level_list_loaded and removed by level_unload_sublevel.
#include "Engine/LevelStreaming.h"
#include "Misc/PackageName.h"       // FPackageName path validation + short name
#include "UObject/Package.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	/**
	 * Parse the raw POST body into a JSON object. Returns null when the body is
	 * empty or not a JSON object (the caller surfaces a structured
	 * `invalid_parameter` error). Same contract as the actor family's ParseBody
	 * — kept here so the parsing shape is identical across the level family.
	 */
	TSharedPtr<FJsonObject> ParseBody(const FString& Body)
	{
		const FString Trimmed = Body.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			// An empty body is a valid "no args" call — treat as an empty object
			// so handlers can resolve optional fields with their defaults.
			return MakeShared<FJsonObject>();
		}
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			return nullptr;
		}
		return Object;
	}

	/** Serialize a JsonObject (or any JsonValue) to a compact string. Returns
	 *  "null" on a null pointer so the result is always valid JSON. Same shape
	 *  as the actor family's WriteJson. */
	FString WriteJson(const TSharedPtr<FJsonValue>& JsonValue)
	{
		if (!JsonValue.IsValid())
		{
			return TEXT("null");
		}
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		if (FJsonSerializer::Serialize(JsonValue, Writer))
		{
			return Out;
		}
		return TEXT("null");
	}

	/** Long package name of a level's outermost package (e.g. /Game/Maps/Arena);
	 *  empty when null or the level has no package (a transient level). */
	FString LevelPackageName(const ULevel* Level)
	{
		if (Level == nullptr)
		{
			return FString();
		}
		const UPackage* Package = Level->GetOutermost();
		return Package != nullptr ? Package->GetName() : FString();
	}

	/** Short, content-browser-style name of a level (e.g. "Arena"); empty when
	 *  the level has no package name. */
	FString LevelShortName(const ULevel* Level)
	{
		const FString PackageName = LevelPackageName(Level);
		return PackageName.IsEmpty() ? FString() : FPackageName::GetShortName(PackageName);
	}

	/**
	 * Serialize one loaded level into the `LevelInfo` payload documented in the
	 * P2.6 contract: { path, name, isCurrent, dirty }. `path` is the long
	 * package name (the path-first identity), `name` is the content-browser
	 * short name, `isCurrent` is whether this level is the editor's current
	 * editing level, and `dirty` is whether its outermost package has unsaved
	 * edits. For streaming sublevels the caller supplies the load/visibility
	 * flags directly (see the list handler).
	 *
	 * Mirrors Unity's scene identity block (path + isDirty), adapted to Unreal's
	 * package-dirty state and the streaming-level surface.
	 */
	TSharedRef<FJsonObject> ToLevelData(ULevel* Level, const UWorld* World)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		const FString PackageName = LevelPackageName(Level);
		Json->SetStringField(TEXT("path"), PackageName);
		Json->SetStringField(TEXT("name"), LevelShortName(Level));
		Json->SetBoolField(TEXT("isCurrent"), World != nullptr && Level == World->GetCurrentLevel());

		// Package dirty bit — the same surface the editor's save-prompt keys
		// off. A transient level (no package) reports dirty=false; callers that
		// need to know a level was never saved should check whether `path` is
		// empty.
		bool bDirty = false;
		if (Level != nullptr)
		{
			if (const UPackage* Package = Level->GetOutermost())
			{
				bDirty = Package->IsDirty();
			}
		}
		Json->SetBoolField(TEXT("dirty"), bDirty);
		return Json;
	}

	/**
	 * Resolve a level asset path (object path like '/Game/Maps/Arena.Arena' or a
	 * plain package name like '/Game/Maps/Arena') to the on-disk .umap filename,
	 * returning false + a structured reason when the path is malformed or no
	 * package exists. Used to turn an expected-miss (typo'd path) into a clean
	 * `invalid_path` / `level_not_found` error BEFORE calling an engine API
	 * (LoadMap) that would otherwise log an engine Error (which Automation
	 * counts as a failure) or silently fall back to a blank map.
	 *
	 * Adapted (read-only) from Unreal-MCP's ResolveLevelFilename.
	 */
	bool ResolveLevelFilename(const FString& AssetPath, FString& OutFilename, FString& OutError)
	{
		FString PackageName = AssetPath;
		if (FPackageName::IsValidObjectPath(AssetPath))
		{
			PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
		}
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = FString::Printf(
				TEXT("'%s' is not a valid level asset path (expected e.g. '/Game/Maps/Arena')."),
				*AssetPath);
			return false;
		}
		if (!FPackageName::DoesPackageExist(PackageName, &OutFilename))
		{
			OutError = FString::Printf(
				TEXT("No level package exists at '%s'."),
				*PackageName);
			return false;
		}
		// DoesPackageExist is content-agnostic: a non-level asset (material,
		// blueprint, ...) also passes. Without this guard LoadMap would log an
		// engine Error. Require the resolved package to be a .umap.
		if (!OutFilename.EndsWith(FPackageName::GetMapPackageExtension(), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(
				TEXT("'%s' is not a level/map asset."),
				*PackageName);
			OutFilename.Reset();
			return false;
		}
		return true;
	}

	/**
	 * Normalise a save-as asset path (object-path or package-name form) to a
	 * long package name, validating it. Returns false + a reason when malformed.
	 * Sets bOutExists when a package already exists at the target so the caller
	 * can surface an overwrite (SaveMap force-overwrites silently otherwise) and
	 * rejects a collision with a non-map asset (which would leave two on-disk
	 * files under one package name).
	 *
	 * Adapted (read-only) from Unreal-MCP's ResolveSaveTargetPackage.
	 */
	bool ResolveSaveTarget(const FString& AssetPath, FString& OutPackageName, bool& bOutExists, FString& OutError)
	{
		OutPackageName = AssetPath;
		if (FPackageName::IsValidObjectPath(AssetPath))
		{
			OutPackageName = FPackageName::ObjectPathToPackageName(AssetPath);
		}
		if (!FPackageName::IsValidLongPackageName(OutPackageName))
		{
			OutError = FString::Printf(
				TEXT("'%s' is not a valid level asset path (expected e.g. '/Game/Maps/Arena')."),
				*AssetPath);
			return false;
		}
		FString ExistingFilename;
		bOutExists = FPackageName::DoesPackageExist(OutPackageName, &ExistingFilename);
		if (bOutExists && !ExistingFilename.EndsWith(FPackageName::GetMapPackageExtension(), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(
				TEXT("'%s' already exists and is not a level/map asset; choose a different path."),
				*OutPackageName);
			return false;
		}
		return true;
	}

	/** JsonValue array from a string array — used for the discardedDirtyLevels
	 *  note and the save result's saved[] list. */
	TArray<TSharedPtr<FJsonValue>> StringsToJsonValues(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Strings.Num());
		for (const FString& S : Strings)
		{
			Out.Add(MakeShared<FJsonValueString>(S));
		}
		return Out;
	}
}

void FUnrealOpenMcpLevelTools::Register(FUnrealOpenMcpToolRegistry& Registry)
{
	// unreal_open_mcp_level_open — open an existing level by content path,
	// replacing the current editor world (FEditorFileUtils::LoadMap via
	// UEditorLoadingAndSavingUtils::LoadMap). The dirty guard refuses the open
	// when the current world has unsaved edits unless `ignore_dirty` is set; a
	// bypass surfaces the discarded packages as a `discardedDirtyLevels` note so
	// an unattended caller still learns what was lost.
	//
	// Mutating (replaces the world). `paths_hint` + `gate` are forward-compat
	// (no-op until P3.5). Structured errors:
	//   - missing_parameter — `path` absent/empty
	//   - invalid_path      — path is not a valid /Game/... package path
	//   - level_not_found   — no .umap package exists at the path
	//   - level_dirty       — current world has unsaved edits (ignore_dirty=false)
	//   - no_editor         — GEditor is null (not running in the editor)
	Registry.Register(
		TEXT("unreal_open_mcp_level_open"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			// level_open REPLACES the world, so there is no current world to
			// probe — check GEditor directly (mirrors the behavior reference).
			if (GEditor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor"),
					TEXT("No editor is available (not running in the Unreal Editor)."));
			}

			const FString Path = Args->HasTypedField<EJson::String>(TEXT("path"))
				? Args->GetStringField(TEXT("path"))
				: FString();
			if (Path.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'path' is required (content path of the level to open, e.g. '/Game/Maps/Arena')."));
			}

			// Dirty guard FIRST — mirrors the spec plan's logic order (dirty
			// check before path resolution). Capturing the dirty packages BEFORE
			// the load replaces the world is the load-bearing part: LoadMap
			// discards them and the engine's confirm dialog is suppressed under
			// -unattended, so this is the only signal. Doing it before path
			// resolution also means the guard is exercised even when the caller
			// hands a path that does not exist (so a dirty-world test does not
			// need a real .umap on disk to reach the guard).
			const TArray<FString> DiscardedDirty = FUnrealOpenMcpLevelDirtyGuard::DirtyMapPackageNames();
			const bool bIgnoreDirty = Args->HasTypedField<EJson::Boolean>(TEXT("ignore_dirty"))
				&& Args->GetBoolField(TEXT("ignore_dirty"));
			if (DiscardedDirty.Num() > 0 && !bIgnoreDirty)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("level_dirty"),
					FString::Printf(
						TEXT("The current level has unsaved edits (%d package(s)). "
						     "Save first, or set ignore_dirty=true to discard them and open '%s'."),
						DiscardedDirty.Num(), *Path));
			}

			// Normalise to a long package name + resolve the .umap on disk.
			// Probing existence up front turns an expected-miss into a clean
			// structured error instead of an engine Error log (which Automation
			// would count as a failure) or a load of the wrong file.
			FString Filename;
			FString ResolveError;
			if (!ResolveLevelFilename(Path, Filename, ResolveError))
			{
				// Distinguish a malformed path (invalid_path) from a package that
				// simply does not exist (level_not_found) so an agent can branch.
				const bool bMalformed = ResolveError.Contains(TEXT("is not a valid level asset path"));
				return FUnrealOpenMcpToolDispatchResult::Fail(
					bMalformed ? TEXT("invalid_path") : TEXT("level_not_found"),
					ResolveError);
			}

			UWorld* Opened = UEditorLoadingAndSavingUtils::LoadMap(Filename);
			if (Opened == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("level_not_found"),
					FString::Printf(
						TEXT("Failed to open level '%s' (LoadMap returned null)."),
						*Path));
			}

			TSharedRef<FJsonObject> Result = ToLevelData(Opened->PersistentLevel, Opened);
			// A freshly opened level is never dirty.
			Result->SetBoolField(TEXT("dirty"), false);
			if (DiscardedDirty.Num() > 0)
			{
				// The bypass case: surface what was discarded so the caller knows.
				Result->SetArrayField(TEXT("discardedDirtyLevels"), StringsToJsonValues(DiscardedDirty));
			}
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	// unreal_open_mcp_level_save — save the current level in place, or save-as
	// the persistent level to a new content path via `path` (save-as uses
	// UEditorLoadingAndSavingUtils::SaveMap; in-place uses SaveCurrentLevel).
	// A transient/never-saved level with no `path` returns `save_failed`
	// (provide `path` to give it a location) — without this guard the in-place
	// path would raise a modal Save-As file dialog that blocks the game thread.
	//
	// Mutating. `paths_hint` + `gate` are forward-compat (no-op until P3.5).
	// Structured errors:
	//   - no_editor_world  — no GEditor / editor world
	//   - invalid_path     — save-as `path` is not a valid package path
	//   - save_failed      — SaveMap/SaveCurrentLevel returned false, or the
	//                        current level was never saved and no `path` given
	Registry.Register(
		TEXT("unreal_open_mcp_level_save"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			// Reuse the actor family's editor-world accessor via a local probe:
			// GEditor->GetEditorWorldContext().World() is the current editor
			// world the save targets.
			if (GEditor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			const FString SavePath = Args->HasTypedField<EJson::String>(TEXT("path"))
				? Args->GetStringField(TEXT("path"))
				: FString();

			TArray<FString> SavedPackages;

			if (!SavePath.IsEmpty())
			{
				// Save-as: validate the target + probe for a pre-existing level.
				// SaveMap force-overwrites an existing .umap silently, so detect
				// the collision and report it (we still proceed — overwriting is
				// the documented save-as contract — but the caller sees
				// `overwrote`).
				FString SavedPackage;
				bool bOverwrote = false;
				FString SaveError;
				if (!ResolveSaveTarget(SavePath, SavedPackage, bOverwrote, SaveError))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_path"),
						SaveError);
				}
				if (!UEditorLoadingAndSavingUtils::SaveMap(World, SavedPackage))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("save_failed"),
						FString::Printf(
							TEXT("Failed to save the level to '%s' (invalid path or save was declined)."),
							*SavedPackage));
				}
				SavedPackages.Add(SavedPackage);

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetArrayField(TEXT("saved"), StringsToJsonValues(SavedPackages));
				Result->SetNumberField(TEXT("count"), SavedPackages.Num());
				Result->SetStringField(TEXT("savedPath"), SavedPackage);
				Result->SetBoolField(TEXT("overwrote"), bOverwrote);
				return FUnrealOpenMcpToolDispatchResult::Ok(
					WriteJson(MakeShared<FJsonValueObject>(Result)));
			}

			// Save-in-place. Guard the transient/never-saved case BEFORE calling
			// SaveCurrentLevel: for a level with no on-disk package,
			// FEditorFileUtils::SaveLevel falls into the interactive Save-As
			// file dialog (modal UI that would block the game thread / bridge).
			// Probing the current level's package existence keeps this
			// headless-safe in an interactive editor too.
			ULevel* CurrentLevel = World->GetCurrentLevel();
			const FString CurrentPackage = LevelPackageName(CurrentLevel);
			if (CurrentPackage.IsEmpty() || !FPackageName::DoesPackageExist(CurrentPackage))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("save_failed"),
					TEXT("The current level has never been saved (it has no on-disk location yet). "
					     "Pass 'path' to save it to a new location."));
			}

			if (!UEditorLoadingAndSavingUtils::SaveCurrentLevel())
			{
				// The level has a real package on disk; a false return now
				// indicates a checkout or write failure rather than the transient
				// case.
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("save_failed"),
					TEXT("The current level could not be saved in place (a checkout or write failure)."));
			}
			SavedPackages.Add(CurrentPackage);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("saved"), StringsToJsonValues(SavedPackages));
			Result->SetNumberField(TEXT("count"), SavedPackages.Num());
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	// unreal_open_mcp_level_list_loaded — read-only enumeration of the levels
	// loaded in the current editor world: the persistent level first, then any
	// streaming sublevels. Each entry carries path-first identity
	// ({ path, name, isCurrent, dirty }) plus load/visibility flags for
	// sublevels so an agent can branch on a sublevel's state without a second
	// call.
	//
	// Read-only — no gate. Structured errors:
	//   - no_editor_world — no GEditor / editor world
	//   - invalid_parameter — malformed body
	Registry.Register(
		TEXT("unreal_open_mcp_level_list_loaded"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			if (GEditor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			TArray<TSharedPtr<FJsonValue>> Levels;

			// Persistent level first — it is always loaded + visible.
			if (World->PersistentLevel != nullptr)
			{
				TSharedRef<FJsonObject> Entry = ToLevelData(World->PersistentLevel, World);
				Entry->SetBoolField(TEXT("isLoaded"), true);
				Entry->SetBoolField(TEXT("isVisible"), true);
				Entry->SetBoolField(TEXT("isPersistent"), true);
				Levels.Add(MakeShared<FJsonValueObject>(Entry));
			}

			// Streaming sublevels. A sublevel that is not currently loaded has
			// a null loaded-level pointer; report its requested load/visibility
			// flags + its package path regardless so the agent can see the
			// sublevel exists even when unloaded. The dirty bit only applies to
			// a loaded sublevel (a null one has no package to be dirty).
			for (ULevelStreaming* Streaming : World->GetStreamingLevels())
			{
				if (Streaming == nullptr)
				{
					continue;
				}
				ULevel* Loaded = Streaming->GetLoadedLevel();
				const FString Package = Streaming->GetWorldAssetPackageName();

				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("path"), Package);
				Entry->SetStringField(TEXT("name"), FPackageName::GetShortName(Package));
				Entry->SetBoolField(TEXT("isCurrent"), Loaded != nullptr && Loaded == World->GetCurrentLevel());
				Entry->SetBoolField(TEXT("isPersistent"), false);
				Entry->SetBoolField(TEXT("isLoaded"), Loaded != nullptr);
				Entry->SetBoolField(TEXT("isVisible"), Streaming->GetShouldBeVisibleInEditor());

				bool bDirty = false;
				if (Loaded != nullptr)
				{
					if (const UPackage* PackageObj = Loaded->GetOutermost())
					{
						bDirty = PackageObj->IsDirty();
					}
				}
				Entry->SetBoolField(TEXT("dirty"), bDirty);
				Levels.Add(MakeShared<FJsonValueObject>(Entry));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("levels"), Levels);
			Result->SetNumberField(TEXT("count"), Levels.Num());
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	// unreal_open_mcp_level_set_current — set the current editing level (the
	// one new actors are added to) by short name OR full package path
	// (UEditorLevelUtils::MakeLevelCurrent). The package path is unambiguous;
	// a short name matching more than one loaded level is an error (pass the
	// package path to disambiguate).
	//
	// Mutating. `paths_hint` + `gate` are forward-compat (no-op until P3.5).
	// Structured errors:
	//   - missing_parameter — `path` absent/empty
	//   - no_editor_world   — no GEditor / editor world
	//   - level_not_found   — no loaded level matches the name/path
	//   - ambiguous_name    — short name matches multiple loaded levels
	//   - set_current_failed — MakeLevelCurrent did not take effect (locked)
	Registry.Register(
		TEXT("unreal_open_mcp_level_set_current"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			if (GEditor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			const FString Name = Args->HasTypedField<EJson::String>(TEXT("path"))
				? Args->GetStringField(TEXT("path"))
				: FString();
			if (Name.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'path' is required (short name or full package path of a loaded level)."));
			}

			// Match by full package path OR short name. The package path is
			// unambiguous; a short name matching more than one loaded level is
			// an error. Adapted (read-only) from the behavior reference's
			// short-name-vs-package-path disambiguation.
			ULevel* Target = nullptr;
			int32 ShortNameMatches = 0;
			for (ULevel* Level : World->GetLevels())
			{
				if (Level == nullptr)
				{
					continue;
				}
				if (LevelPackageName(Level).Equals(Name, ESearchCase::IgnoreCase))
				{
					// An exact package-path hit is decisive.
					Target = Level;
					ShortNameMatches = 1;
					break;
				}
				if (LevelShortName(Level).Equals(Name, ESearchCase::IgnoreCase))
				{
					Target = Level;
					++ShortNameMatches;
				}
			}
			if (ShortNameMatches > 1)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("ambiguous_name"),
					FString::Printf(
						TEXT("'%s' matches multiple loaded levels; pass the full package path (e.g. '/Game/Maps/Arena') to disambiguate."),
						*Name));
			}
			if (Target == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("level_not_found"),
					FString::Printf(
						TEXT("No loaded level matches '%s' (use level_list_loaded to see the available names)."),
						*Name));
			}

			if (World->GetCurrentLevel() == Target)
			{
				// Idempotent: already the current level. Return the identity so
				// the shape is uniform across the no-op and the real switch.
				TSharedRef<FJsonObject> Result = ToLevelData(Target, World);
				return FUnrealOpenMcpToolDispatchResult::Ok(
					WriteJson(MakeShared<FJsonValueObject>(Result)));
			}

			// MakeLevelCurrent updates the editor's current-level state; it is a
			// no-op for a locked level unless forced — we do not force here.
			UEditorLevelUtils::MakeLevelCurrent(Target, /*bEvenIfLocked*/ false);
			if (World->GetCurrentLevel() != Target)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("set_current_failed"),
					FString::Printf(
						TEXT("Could not make '%s' the current level (it may be locked)."),
						*LevelShortName(Target)));
			}

			TSharedRef<FJsonObject> Result = ToLevelData(Target, World);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	// unreal_open_mcp_level_unload_sublevel — unload (remove from the world) a
	// loaded streaming sublevel by short name OR full package path
	// (UEditorLevelUtils::RemoveLevelFromWorld). The persistent level cannot be
	// unloaded. The sublevel's unsaved state is captured BEFORE the remove and
	// surfaced as `wasDirty` (mirrors the family's discarded-edits convention).
	//
	// Mutating. `paths_hint` + `gate` are forward-compat (no-op until P3.5).
	// Structured errors:
	//   - missing_parameter — `path` absent/empty
	//   - no_editor_world   — no GEditor / editor world
	//   - level_not_found   — no streaming sublevel matches the name/path
	//   - ambiguous_name    — short name matches multiple sublevels
	//   - persistent_level  — the name resolves to the persistent level
	//   - not_loaded        — the matched sublevel is not currently loaded
	//   - unload_failed     — RemoveLevelFromWorld returned false
	Registry.Register(
		TEXT("unreal_open_mcp_level_unload_sublevel"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			if (GEditor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			const FString Name = Args->HasTypedField<EJson::String>(TEXT("path"))
				? Args->GetStringField(TEXT("path"))
				: FString();
			if (Name.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'path' is required (short name or full package path of a streaming sublevel)."));
			}

			// Search the streaming sublevels FIRST (by short name OR full package
			// path), before the persistent-level guard, so a sublevel that shares
			// the persistent level's short name stays reachable via its package
			// path. A short name matching multiple sublevels is an error.
			ULevelStreaming* Match = nullptr;
			int32 ShortNameMatches = 0;
			for (ULevelStreaming* Streaming : World->GetStreamingLevels())
			{
				if (Streaming == nullptr)
				{
					continue;
				}
				const FString Package = Streaming->GetWorldAssetPackageName();
				if (Package.Equals(Name, ESearchCase::IgnoreCase))
				{
					Match = Streaming;
					ShortNameMatches = 1;
					break;
				}
				if (FPackageName::GetShortName(Package).Equals(Name, ESearchCase::IgnoreCase))
				{
					Match = Streaming;
					++ShortNameMatches;
				}
			}
			if (ShortNameMatches > 1)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("ambiguous_name"),
					FString::Printf(
						TEXT("'%s' matches multiple streaming sublevels; pass the full package path to disambiguate."),
						*Name));
			}
			if (Match == nullptr)
			{
				// Not a sublevel: explain the persistent-level case specifically,
				// else a plain level_not_found.
				if (World->PersistentLevel != nullptr
					&& (LevelShortName(World->PersistentLevel).Equals(Name, ESearchCase::IgnoreCase)
						|| LevelPackageName(World->PersistentLevel).Equals(Name, ESearchCase::IgnoreCase)))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("persistent_level"),
						FString::Printf(
							TEXT("'%s' is the persistent level and cannot be unloaded."),
							*Name));
				}
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("level_not_found"),
					FString::Printf(
						TEXT("No streaming sublevel matches '%s' (use level_list_loaded to see the available names)."),
						*Name));
			}

			ULevel* Loaded = Match->GetLoadedLevel();
			if (Loaded == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("not_loaded"),
					FString::Printf(
						TEXT("Sublevel '%s' is not currently loaded; nothing to unload."),
						*Name));
			}

			// Capture the sublevel's unsaved state BEFORE removing it:
			// RemoveLevelFromWorld discards a dirty sublevel's edits with no
			// prompt under -unattended, so surface `wasDirty`.
			const bool bWasDirty = Loaded->GetOutermost() != nullptr && Loaded->GetOutermost()->IsDirty();

			if (!UEditorLevelUtils::RemoveLevelFromWorld(Loaded))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("unload_failed"),
					FString::Printf(
						TEXT("Failed to unload sublevel '%s'."),
						*Name));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("path"), Match->GetWorldAssetPackageName());
			Result->SetStringField(TEXT("name"), FPackageName::GetShortName(Match->GetWorldAssetPackageName()));
			Result->SetBoolField(TEXT("wasDirty"), bWasDirty);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	UE_LOG(
		LogUnrealOpenMcp,
		Log,
		TEXT("[Unreal Open MCP] level tools registered: unreal_open_mcp_level_open, unreal_open_mcp_level_save, unreal_open_mcp_level_list_loaded, unreal_open_mcp_level_set_current, unreal_open_mcp_level_unload_sublevel"));
}
