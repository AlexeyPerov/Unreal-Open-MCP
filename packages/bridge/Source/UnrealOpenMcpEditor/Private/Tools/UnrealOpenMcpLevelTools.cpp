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
// P2.7 — TActorIterator<AActor> for the level_get_data whole-world sweep.
#include "EngineUtils.h"
// P2.7 — AActor::GetActorLabel / GetFolderName for the level_get_data roster.
#include "GameFramework/Actor.h"

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

	// ---- P2.7: level_get_data helpers -------------------------------------

	/** Profile for level_get_data's token-budget output. Mirrors Unity's
	 *  scene_get_data detail/profile axis: compact = identity only, balanced =
	 *  + folder + class short name, full = + transform + components list.
	 *  Keeping the enum local (not on the header) because the profile is an
	 *  output concern of this handler only. */
	enum class ELevelProfile : uint8
	{
		Compact,
		Balanced,
		Full,
	};

	/** Parse the `profile` field (case-insensitive). Unknown / missing values
	 *  fall back to Compact — the cheapest read, matching Unity's default. */
	ELevelProfile ParseProfile(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid() || !Args->HasTypedField<EJson::String>(TEXT("profile")))
		{
			return ELevelProfile::Compact;
		}
		const FString Raw = Args->GetStringField(TEXT("profile"));
		if (Raw.Equals(TEXT("balanced"), ESearchCase::IgnoreCase))
		{
			return ELevelProfile::Balanced;
		}
		if (Raw.Equals(TEXT("full"), ESearchCase::IgnoreCase))
		{
			return ELevelProfile::Full;
		}
		return ELevelProfile::Compact;
	}

	/** Hard ceiling on the actor roster so a huge level never floods the
	 *  response. Mirrors Unity's max_nodes guard at a smaller default (actors are
	 *  coarser than GameObjects — a typical UE level has far fewer top-level
	 *  actors than a Unity scene has GameObjects). */
	constexpr int32 LevelGetDataDefaultMaxActors = 50;
	constexpr int32 LevelGetDataMaxActorsCap = 200;

	/** Resolve the `max_actors` argument into [1, Cap], falling back to the
	 *  default when absent or loosely typed. Mirrors ResolveMaxResults in the
	 *  actor family so the contract is identical across families. */
	int32 ResolveMaxActors(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid() || !Args->HasTypedField<EJson::Number>(TEXT("max_actors")))
		{
			return LevelGetDataDefaultMaxActors;
		}
		const int32 Requested = static_cast<int32>(Args->GetNumberField(TEXT("max_actors")));
		if (Requested < 1)
		{
			return LevelGetDataDefaultMaxActors;
		}
		return FMath::Min(Requested, LevelGetDataMaxActorsCap);
	}

	/** Resolve the `page_size` argument; 0 / invalid = unpaged (return the whole
	 *  bounded roster in one response, like Unity's omit-page_size path). */
	int32 ResolvePageSize(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid() || !Args->HasTypedField<EJson::Number>(TEXT("page_size")))
		{
			return 0;
		}
		const int32 Requested = static_cast<int32>(Args->GetNumberField(TEXT("page_size")));
		if (Requested < 1)
		{
			return 0;
		}
		return Requested;
	}

	/** Decode an opaque pagination cursor. The cursor is the zero-based actor
	 *  index encoded as a decimal string — simple, debuggable, and stateless
	 *  (the handler does not hold per-session pagination state). Returns 0 on
	 *  absent/invalid input (start of the stream). */
	int32 DecodeCursor(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid() || !Args->HasTypedField<EJson::String>(TEXT("cursor")))
		{
			return 0;
		}
		const FString Raw = Args->GetStringField(TEXT("cursor"));
		if (!Raw.IsNumeric())
		{
			return 0;
		}
		const int32 Index = FCString::Atoi(*Raw);
		return Index < 0 ? 0 : Index;
	}

	/** Serialize one actor into the level_get_data roster entry, scoped by the
	 *  profile:
	 *    compact  — { label, name }
	 *    balanced — + folder, class (short name)
	 *    full     — + transform + components (short-name array)
	 *
	 *  Adapted from the actor family's ToActorData (same label/class/transform
	 *  fields) at a smaller footprint: compact is the default here (identity
	 *  only), whereas ToActorData always emits the transform + class. The
	 *  roster is a survey, not a per-actor drill-down — the agent chains into
	 *  actor_find / actor_modify for the full payload. */
	TSharedRef<FJsonObject> ActorRosterEntry(AActor* Actor, ELevelProfile Profile)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Entry->SetStringField(TEXT("name"), Actor->GetName());

		if (Profile >= ELevelProfile::Balanced)
		{
			// Outliner folder path ("/Folder/SubFolder" or "" when ungrouped).
			// GetFolderPath is editor-only (WITH_EDITORONLY_DATA) — safe here
			// because this whole module is an Editor module.
			Entry->SetStringField(TEXT("folder"), Actor->GetFolderPath().ToString());
			Entry->SetStringField(
				TEXT("class"),
				Actor->GetClass() ? Actor->GetClass()->GetName() : FString());
		}

		if (Profile >= ELevelProfile::Full)
		{
			// Transform — same shape as the actor family's TransformToJson.
			const FTransform T = Actor->GetTransform();
			TSharedRef<FJsonObject> TransformJson = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> Loc = MakeShared<FJsonObject>();
			Loc->SetNumberField(TEXT("x"), T.GetLocation().X);
			Loc->SetNumberField(TEXT("y"), T.GetLocation().Y);
			Loc->SetNumberField(TEXT("z"), T.GetLocation().Z);
			TransformJson->SetObjectField(TEXT("location"), Loc);
			TSharedRef<FJsonObject> Rot = MakeShared<FJsonObject>();
			const FRotator R = T.Rotator();
			Rot->SetNumberField(TEXT("pitch"), R.Pitch);
			Rot->SetNumberField(TEXT("yaw"), R.Yaw);
			Rot->SetNumberField(TEXT("roll"), R.Roll);
			TransformJson->SetObjectField(TEXT("rotation"), Rot);
			TSharedRef<FJsonObject> Scale = MakeShared<FJsonObject>();
			Scale->SetNumberField(TEXT("x"), T.GetScale3D().X);
			Scale->SetNumberField(TEXT("y"), T.GetScale3D().Y);
			Scale->SetNumberField(TEXT("z"), T.GetScale3D().Z);
			TransformJson->SetObjectField(TEXT("scale"), Scale);
			Entry->SetObjectField(TEXT("transform"), TransformJson);

			// Components — short-name array (name + class), mirroring the actor
			// family's component roster shape at the same granularity.
			TArray<TSharedPtr<FJsonValue>> Components;
			for (UActorComponent* Component : Actor->GetComponents())
			{
				if (Component == nullptr)
				{
					continue;
				}
				TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
				C->SetStringField(TEXT("name"), Component->GetName());
				C->SetStringField(
					TEXT("class"),
					Component->GetClass() ? Component->GetClass()->GetName() : FString());
				Components.Add(MakeShared<FJsonValueObject>(C));
			}
			Entry->SetArrayField(TEXT("components"), Components);
		}

		return Entry;
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

	// unreal_open_mcp_level_get_data — read-only actor roster of the current
	// editor world (or a loaded streaming sublevel named by `path`), with a
	// token-budget profile (compact/balanced/full) + pagination
	// (page_size/cursor). The Unreal analog of Unity's scene_get_data.
	//
	// World Partition: when the world is a PartitionedWorld, only actors in
	// LOADED cells are visible to the editor's actor iterator, so the result
	// carries `worldPartition:true` and a `partitionScope:"loaded-cells-only"`
	// note so the caller knows the roster is not the complete actor set.
	//
	// Read-only — no gate. Structured errors:
	//   - invalid_parameter — malformed body
	//   - no_editor_world   — no GEditor / editor world
	//   - level_not_found   — `path` matches no loaded (sub)level
	//   - invalid_cursor    — cursor decodes to an index past the roster end
	Registry.Register(
		TEXT("unreal_open_mcp_level_get_data"),
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

			// When `path` is supplied, scope the sweep to that loaded
			// (sub)level's actors. A miss is a clean level_not_found (not a
			// silent fall-through to the whole world — the caller asked for a
			// specific level). Matches by full package path or short name; a
			// short name matching multiple loaded levels is rejected.
			ULevel* ScopeLevel = World->PersistentLevel;
			const FString PathArg = Args->HasTypedField<EJson::String>(TEXT("path"))
				? Args->GetStringField(TEXT("path"))
				: FString();
			if (!PathArg.IsEmpty())
			{
				ULevel* Match = nullptr;
				int32 ShortNameMatches = 0;
				for (ULevel* Level : World->GetLevels())
				{
					if (Level == nullptr)
					{
						continue;
					}
					if (LevelPackageName(Level).Equals(PathArg, ESearchCase::IgnoreCase))
					{
						Match = Level;
						ShortNameMatches = 1;
						break;
					}
					if (LevelShortName(Level).Equals(PathArg, ESearchCase::IgnoreCase))
					{
						Match = Level;
						++ShortNameMatches;
					}
				}
				if (ShortNameMatches > 1)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("ambiguous_name"),
						FString::Printf(
							TEXT("'%s' matches multiple loaded levels; pass the full package path to disambiguate."),
							*PathArg));
				}
				if (Match == nullptr)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("level_not_found"),
						FString::Printf(
							TEXT("No loaded level matches '%s' (use level_list_loaded to see the available names)."),
							*PathArg));
				}
				ScopeLevel = Match;
			}

			const ELevelProfile Profile = ParseProfile(Args);
			const int32 MaxActors = ResolveMaxActors(Args);
			const int32 PageSize = ResolvePageSize(Args);
			const int32 Cursor = DecodeCursor(Args);

			// Materialize the actor roster in label order (stable, deterministic
			// across calls — the editor's actor array order is not guaranteed).
			// Only actors whose outer level is the scope level are included, so
			// a `path`-scoped read of a sublevel does not bleed the persistent
			// level's actors in. Sorting by label also makes pagination cursors
			// stable: the same cursor resolves the same actor across calls.
			TArray<AActor*> Actors;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (Actor == nullptr)
				{
					continue;
				}
				if (Actor->GetLevel() != ScopeLevel)
				{
					continue;
				}
				Actors.Add(Actor);
			}
			Actors.Sort([](const AActor& A, const AActor& B)
			{
				return A.GetActorLabel().Compare(B.GetActorLabel(), ESearchCase::IgnoreCase) < 0;
			});

			const int32 Total = Actors.Num();

			// Apply the hard cap first (bounds token cost), then the pagination
			// window. The cap clamps the effective stream length; the cursor +
			// page_size slice a window out of the capped stream. next_cursor is
			// null when the window reaches the end of the stream.
			const int32 EffectiveTotal = FMath::Min(Total, MaxActors);
			const bool bTruncatedByCap = Total > MaxActors;

			int32 WindowStart = FMath::Clamp(Cursor, 0, EffectiveTotal);
			// A cursor past the end of the (capped) stream is invalid — refuse
			// rather than silently returning an empty page, so a stale cursor
			// from a previous (larger) roster surfaces clearly.
			if (Cursor > EffectiveTotal && EffectiveTotal > 0)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_cursor"),
					FString::Printf(
						TEXT("Cursor %d is past the end of the actor stream (length %d). Omit cursor or pass a value <= %d."),
						Cursor, EffectiveTotal, EffectiveTotal));
			}
			int32 WindowEnd = PageSize > 0
				? FMath::Min(WindowStart + PageSize, EffectiveTotal)
				: EffectiveTotal;

			TArray<TSharedPtr<FJsonValue>> Roster;
			for (int32 i = WindowStart; i < WindowEnd; ++i)
			{
				Roster.Add(MakeShared<FJsonValueObject>(ActorRosterEntry(Actors[i], Profile)));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("path"), LevelPackageName(ScopeLevel));
			Result->SetStringField(TEXT("name"), LevelShortName(ScopeLevel));
			Result->SetStringField(
				TEXT("profile"),
				Profile == ELevelProfile::Full ? TEXT("full")
					: Profile == ELevelProfile::Balanced ? TEXT("balanced")
					: TEXT("compact"));
			Result->SetNumberField(TEXT("actorCount"), Total);
			Result->SetNumberField(TEXT("returned"), Roster.Num());
			Result->SetArrayField(TEXT("actors"), Roster);

			// World Partition: only loaded cells are visible to the actor
			// iterator. Surface the flag + scope so an agent does not mistake a
			// sparse roster for the complete actor set. Greenfield-adjacent per
			// the P2.7 plan (no Unity equivalent).
			const bool bIsPartitioned = World->IsPartitionedWorld();
			Result->SetBoolField(TEXT("worldPartition"), bIsPartitioned);
			if (bIsPartitioned)
			{
				Result->SetStringField(
					TEXT("partitionScope"),
					TEXT("loaded-cells-only"));
			}

			Result->SetBoolField(TEXT("truncated"), bTruncatedByCap);

			// Pagination block — present whenever page_size is set, mirroring
			// Unity's scene_get_data pagination block shape. next_cursor is the
			// next window start, or null when the window reached the stream end.
			if (PageSize > 0)
			{
				TSharedRef<FJsonObject> Pagination = MakeShared<FJsonObject>();
				Pagination->SetNumberField(TEXT("page_size"), PageSize);
				Pagination->SetNumberField(TEXT("cursor"), WindowStart);
				if (WindowEnd < EffectiveTotal)
				{
					Pagination->SetStringField(TEXT("next_cursor"), FString::FromInt(WindowEnd));
				}
				else
				{
					Pagination->SetStringField(TEXT("next_cursor"), FString());
				}
				Result->SetObjectField(TEXT("pagination"), Pagination);
			}

			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	// unreal_open_mcp_level_create — create a new, empty (or template-seeded)
	// level and make it the active editor world (replacing the current one),
	// optionally persisting it to a content path. The Unreal analog of Unity's
	// scene_create.
	//
	// `template` accepts `blank` (default) | `default` (basic lighting + sky —
	// uses the project's default map template when available) OR an asset path
	// of an existing level to seed from (e.g. '/Game/Maps/Templates/Base'). A
	// non-existent asset-path template is rejected with level_not_found BEFORE
	// the world is replaced (NewMapFromTemplate silently falls back to a blank
	// map on a missing template, which would otherwise be a misleading success).
	//
	// When `path` is supplied, the new level is saved to that content path
	// (SaveMap). A collision with an existing NON-LEVEL asset is rejected
	// (path_already_exists) — a collision with an existing .umap is an
	// overwrite (surfaced as `overwrote:true`), matching the level_save save-as
	// contract.
	//
	// Mutating (replaces the world). `paths_hint` + `gate` are forward-compat
	// (no-op until P3.5). The dirty guard mirrors level_open: the current
	// world's unsaved edits are captured before the replace and surfaced as
	// `discardedDirtyLevels` (only when ignore_dirty bypasses the guard).
	//
	// Structured errors:
	//   - missing_parameter — `path` absent/empty when saving
	//   - invalid_path      — save `path` is not a valid package path
	//   - path_already_exists — save `path` collides with a non-level asset
	//   - level_not_found   — `template` asset path does not resolve to a .umap
	//   - create_failed     — GEditor->NewMap / NewMapFromTemplate / SaveMap fail
	//   - level_dirty       — current world has unsaved edits (ignore_dirty=false)
	//   - no_editor         — GEditor is null (not running in the editor)
	Registry.Register(
		TEXT("unreal_open_mcp_level_create"),
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
					TEXT("no_editor"),
					TEXT("No editor is available (not running in the Unreal Editor)."));
			}

			const FString SavePath = Args->HasTypedField<EJson::String>(TEXT("path"))
				? Args->GetStringField(TEXT("path"))
				: FString();
			const FString TemplateRaw = Args->HasTypedField<EJson::String>(TEXT("template"))
				? Args->GetStringField(TEXT("template"))
				: FString("blank");
			const bool bOpenAfterCreate = !Args->HasTypedField<EJson::Boolean>(TEXT("open_after_create"))
				|| Args->GetBoolField(TEXT("open_after_create"));

			// Resolve the template FIRST. "blank" and "default" are the named
			// presets; anything else is treated as an asset path to seed from.
			// An asset-path template is probed up front so a typo'd path fails
			// fast with a structured error instead of a misleading
			// "success + empty world" (NewMapFromTemplate's silent fallback).
			FString TemplateFilename;
			const bool bTemplateIsAssetPath =
				!TemplateRaw.Equals(TEXT("blank"), ESearchCase::IgnoreCase)
				&& !TemplateRaw.Equals(TEXT("default"), ESearchCase::IgnoreCase);
			if (bTemplateIsAssetPath)
			{
				FString ResolveError;
				if (!ResolveLevelFilename(TemplateRaw, TemplateFilename, ResolveError))
				{
					const bool bMalformed = ResolveError.Contains(TEXT("is not a valid level asset path"));
					return FUnrealOpenMcpToolDispatchResult::Fail(
						bMalformed ? TEXT("invalid_path") : TEXT("level_not_found"),
						FString::Printf(
							TEXT("Could not create from template '%s': %s"),
							*TemplateRaw, *ResolveError));
				}
			}

			// Validate the save target BEFORE replacing the world. This is pure
			// validation (no side effects); doing it up front means a malformed
			// `path` fails fast instead of destroying the caller's open world
			// (and its unsaved edits) only to then reject the bad path. A
			// collision with a non-level asset is rejected outright.
			FString SavedPackage;
			bool bOverwrote = false;
			if (!SavePath.IsEmpty())
			{
				FString SaveError;
				if (!ResolveSaveTarget(SavePath, SavedPackage, bOverwrote, SaveError))
				{
					// ResolveSaveTarget rejects a non-level collision with a
					// message containing "already exists and is not a level";
					// surface that as path_already_exists so an agent can branch.
					const bool bCollision = SaveError.Contains(TEXT("already exists and is not a level"));
					return FUnrealOpenMcpToolDispatchResult::Fail(
						bCollision ? TEXT("path_already_exists") : TEXT("invalid_path"),
						SaveError);
				}
			}

			// Dirty guard — same contract as level_open. Capture the dirty
			// packages BEFORE the world is replaced; refuse unless ignore_dirty
			// is set. The guard fires after template + path validation so a
			// malformed-args call does not need a dirty world to reach the
			// guard (mirrors level_open's ordering).
			const TArray<FString> DiscardedDirty = FUnrealOpenMcpLevelDirtyGuard::DirtyMapPackageNames();
			const bool bIgnoreDirty = Args->HasTypedField<EJson::Boolean>(TEXT("ignore_dirty"))
				&& Args->GetBoolField(TEXT("ignore_dirty"));
			if (DiscardedDirty.Num() > 0 && !bIgnoreDirty)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("level_dirty"),
					FString::Printf(
						TEXT("The current level has unsaved edits (%d package(s)). "
						     "Save first, or set ignore_dirty=true to discard them and create the new level."),
						DiscardedDirty.Num()));
			}

			// Create the new world. "default" uses GEditor->NewMap (blank
			// in-memory world — the same surface FAutomationEditorCommonUtils
			// uses); a template asset path uses NewMapFromTemplate with the
			// resolved on-disk filename. "blank" is the same as "default" in
			// this port's scope (the project's default-map template seeding
			// would require project-specific knowledge that is out of scope for
			// P2.7; documented as an intentional delta in the plan).
			UWorld* NewWorld = nullptr;
			if (bTemplateIsAssetPath)
			{
				NewWorld = UEditorLoadingAndSavingUtils::NewMapFromTemplate(
					TemplateFilename, /*bSaveExistingMap*/ false);
			}
			else
			{
				NewWorld = GEditor->NewMap();
			}
			if (NewWorld == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("create_failed"),
					TEXT("Failed to create a new level (the editor returned a null world)."));
			}

			// Persist to disk when a save path was given. SaveMap force-
			// overwrites an existing .umap silently — the collision was already
			// detected into bOverwrote above.
			bool bSaved = false;
			if (!SavePath.IsEmpty())
			{
				bSaved = UEditorLoadingAndSavingUtils::SaveMap(NewWorld, SavedPackage);
				if (!bSaved)
				{
					// The world was already replaced (and the previous world's
					// unsaved edits discarded) before this save attempt; surface
					// the discarded packages in the message so the signal is not
					// lost.
					FString DiscardedSuffix;
					if (DiscardedDirty.Num() > 0)
					{
						DiscardedSuffix = FString::Printf(
							TEXT(" Unsaved edits from the previous world were already discarded: %s."),
							*FString::Join(DiscardedDirty, TEXT(", ")));
					}
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("create_failed"),
						FString::Printf(
							TEXT("Created the level but failed to save it to '%s' (invalid path or save was declined).%s"),
							*SavedPackage, *DiscardedSuffix));
				}
			}

			// open_after_create=false: the create already replaced the world
			// (NewMap/NewMapFromTemplate make it current), so "open" here is a
			// no-op signal — the flag is accepted for schema parity with Unity's
			// scene_create but always effectively true in P2.7 (documented as a
			// delta: there is no additive create in P2).
			(void)bOpenAfterCreate;

			TSharedRef<FJsonObject> Result = ToLevelData(NewWorld->PersistentLevel, NewWorld);
			Result->SetStringField(TEXT("template"), TemplateRaw);
			Result->SetBoolField(TEXT("saved"), bSaved);
			if (bSaved)
			{
				Result->SetStringField(TEXT("savedPath"), SavedPackage);
				Result->SetBoolField(TEXT("overwrote"), bOverwrote);
			}
			if (DiscardedDirty.Num() > 0)
			{
				Result->SetArrayField(TEXT("discardedDirtyLevels"), StringsToJsonValues(DiscardedDirty));
			}
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	UE_LOG(
		LogUnrealOpenMcp,
		Log,
		TEXT("[Unreal Open MCP] level tools registered: unreal_open_mcp_level_open, unreal_open_mcp_level_save, unreal_open_mcp_level_list_loaded, unreal_open_mcp_level_set_current, unreal_open_mcp_level_unload_sublevel, unreal_open_mcp_level_get_data, unreal_open_mcp_level_create"));
}
