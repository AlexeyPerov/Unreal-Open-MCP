// Asset-tool family — see header for the find/get-data + CRUD contracts and
// the shared helpers the later P4 mutators build on. This file owns the two
// read-only handlers (asset_find, asset_get_data), four mutating CRUD
// handlers (asset_create_folder, asset_copy, asset_move, asset_delete), the
// read-only refresh handler, plus the shared `AssetDataToJson` serializer,
// the `GetAssetRegistry` accessor, the `NormalizeContentPath` /
// `SplitObjectPath` resolvers, and the `IsWritableContentRoot` predicate.
//
// Arg parsing + output mirror the level family: each handler parses the raw
// POST body into an FJsonObject and emits a pre-serialized JSON string handed
// to FUnrealOpenMcpToolDispatchResult::Output. The registry/dispatch spine
// stays raw-body — only the handler layer parses.
//
// Behavior reference (read-only): Unreal-MCP's asset handlers
// (UnrealMcpAssetTools.cpp — asset-find / asset-get-data / asset-create-
// folder / asset-copy / asset-move / asset-delete / asset-refresh). The
// filter-shape, the short-name substring post-filter, the deterministic
// object-path sort, the empty-filter → /Game default, the class-path
// validation BEFORE FTopLevelAssetPath construction, the TagsAndValues.ForEach
// serialization, the MakeDirectory idempotent shape, the DuplicateAsset /
// RenameAsset dest-already-exists refusal, the DeleteAsset referencer guard +
// `force` override, and the ScanPathsSynchronous rescan shape were studied
// for correct Unreal editor API usage and adapted to this port's Ok/Fail
// result shape.
#include "Tools/UnrealOpenMcpAssetTools.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "UnrealOpenMcpLog.h"

// AssetRegistry module owns IAssetRegistry + FARFilter + FAssetData — the
// query surface asset_find + asset_get_data read against.
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
// TopLevelAssetPath is the typed form ClassPaths takes; constructed from a
// "/Script/Module.Class" string after shape validation.
#include "UObject/TopLevelAssetPath.h"
// EditorAssetLibrary owns the path-or-name get-data probe (DoesAssetExist +
// FindAssetData) — accepts both "/Game/Foo/M.M" and "/Game/Foo/M", mirroring
// the path-or-name convention the rest of the asset family uses.
#include "EditorAssetLibrary.h"
// FPackageName exposes IsValidLongPackageName / IsValidObjectPath used by
// the path normalization helpers (shared with the level family).
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	/**
	 * Parse the raw POST body into a JSON object. Returns null when the body is
	 * not a valid JSON object (the caller surfaces a structured
	 * `invalid_parameter` error). Same contract as the level family's ParseBody
	 * — kept here so the parsing shape is identical across the asset family.
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
	 *  as the level family's WriteJson. */
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

	/** Access the running AssetRegistry module. Loaded checked — the bridge is
	 *  an Editor module and the AssetRegistry is always present in the editor.
	 *  Mirrors the verify module's resolver accessors (P3.2). */
	IAssetRegistry& GetAssetRegistry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	/**
	 * Reject the engine/script/temp content roots for WRITE operations (used by
	 * P4.2–P4.4 create-folder/import/etc.). The editor APIs would otherwise
	 * happily scribble into /Engine; constrain writes to project / plugin-
	 * mounted roots and return a clean, LLM-actionable error instead of a
	 * generic failure.
	 *
	 * Defined here (next to the path helpers) so the writable-root rule lives
	 * in one place; unused by the read-only find/get-data handlers.
	 *
	 * Read-only behavior reference: Unreal-MCP's IsWritableContentRoot.
	 */
	bool IsWritableContentRoot(const FString& InPath)
	{
		// Reject the exact reserved root ("/Engine") or any path under it
		// ("/Engine/..."), but NOT a sibling mount whose name merely begins
		// with the string (e.g. a "/EngineExtras" plugin root is writable).
		// Matching the trailing slash (plus exact equality) avoids that false
		// reject.
		auto IsReservedRoot = [&InPath](const TCHAR* Root)
		{
			return InPath.Equals(Root) || InPath.StartsWith(FString(Root) + TEXT("/"));
		};
		return !(IsReservedRoot(TEXT("/Engine"))
			|| IsReservedRoot(TEXT("/Script"))
			|| IsReservedRoot(TEXT("/Temp")));
	}

	/**
	 * Normalize a content path to its long package form, accepting either an
	 * object path ("/Game/Foo/M.M") or a package path ("/Game/Foo/M"). Returns
	 * the package path (no trailing dot + asset name); returns the input
	 * verbatim when it is not an object-path form. Used by get-data to accept
	 * both conventions (mirrors the path-or-name convention the asset family
	 * documents).
	 *
	 * Read-only behavior reference: Unreal-MCP's SplitObjectPath.
	 */
	FString NormalizeContentPath(const FString& InPath)
	{
		FString Work = InPath.TrimStartAndEnd();
		if (Work.IsEmpty())
		{
			return FString();
		}
		// Drop a trailing object suffix (".AssetName") if present — we want the
		// package-path form for AssetRegistry lookups.
		int32 DotIndex;
		if (Work.FindChar(TEXT('.'), DotIndex))
		{
			Work.LeftInline(DotIndex);
		}
		while (Work.EndsWith(TEXT("/")))
		{
			Work.LeftChopInline(1);
		}
		return Work;
	}

	/**
	 * Split a destination path into (package-path, asset-name). Accepts either
	 * an object-path form ("/Game/Mat/M_Foo.M_Foo") or a package-path form
	 * ("/Game/Mat/M_Foo"). Returns false when the path has no '/' separator,
	 * starts with '/', or ends with '/' — i.e. it cannot name a valid
	 * destination asset. Used by asset_copy / asset_move to validate the
	 * destination BEFORE handing it to EditorAssetLibrary (the engine would
	 * otherwise log a generic Error + return false; we want a structured
	 * invalid_path error the agent can act on).
	 *
	 * Read-only behavior reference: Unreal-MCP's SplitObjectPath.
	 */
	bool SplitObjectPath(const FString& InPath, FString& OutPackagePath, FString& OutAssetName)
	{
		FString Work = InPath;
		// Drop a trailing object suffix (".AssetName") if present.
		int32 DotIndex;
		if (Work.FindChar(TEXT('.'), DotIndex))
		{
			Work.LeftInline(DotIndex);
		}
		Work.TrimStartAndEndInline();
		while (Work.EndsWith(TEXT("/")))
		{
			Work.LeftChopInline(1);
		}

		int32 SlashIndex;
		// No slash, or slash at position 0 (root only), or slash at the end
		// (trailing '/' stripped above so this catches a path that was ONLY
		// slashes): none of these name a valid destination asset.
		if (!Work.FindLastChar(TEXT('/'), SlashIndex) || SlashIndex == 0 || SlashIndex == Work.Len() - 1)
		{
			return false;
		}
		OutPackagePath = Work.Left(SlashIndex);
		OutAssetName = Work.Mid(SlashIndex + 1);
		return !OutAssetName.IsEmpty() && !OutPackagePath.IsEmpty();
	}

	/**
	 * Serialize one FAssetData into the AssetSummary payload documented in the
	 * P4.1 contract: { name, path, package, class }. `name` is the asset's
	 * short name, `path` is the object path (path-first identity), `package`
	 * is the long package name, and `class` is the asset's class as a
	 * "/Script/Module.Class" top-level path.
	 *
	 * Mirrors Unity's search_assets AssetSummary at adapt fidelity (Unreal
	 * AssetData shape replaces GUID/extension).
	 *
	 * Read-only behavior reference: Unreal-MCP's AssetDataToJson.
	 */
	TSharedRef<FJsonObject> AssetDataToJson(const FAssetData& Data)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Data.AssetName.ToString());
		Obj->SetStringField(TEXT("path"), Data.GetObjectPathString());
		Obj->SetStringField(TEXT("package"), Data.PackageName.ToString());
		Obj->SetStringField(TEXT("class"), Data.AssetClassPath.ToString());
		return Obj;
	}

	// ---- asset_find helpers ---------------------------------------------------

	/** Default page size when `limit` is omitted. Mirrors the Unreal-MCP
	 *  reference; small enough to bound token cost, large enough to be useful
	 *  for a typical folder read. */
	constexpr int32 AssetFindDefaultLimit = 100;
	/** Hard ceiling on the page size so a caller cannot ask for an unbounded
	 *  page. Mirrors the Unreal-MCP reference. */
	constexpr int32 AssetFindMaxLimit = 1000;

	/** Resolve the `offset` argument; negative / invalid → 0. */
	int32 ResolveOffset(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid() || !Args->HasTypedField<EJson::Number>(TEXT("offset")))
		{
			return 0;
		}
		const int32 Requested = static_cast<int32>(Args->GetNumberField(TEXT("offset")));
		return Requested < 0 ? 0 : Requested;
	}

	/** Resolve the `limit` argument into [1, MaxLimit], falling back to the
	 *  default when absent or loosely typed. */
	int32 ResolveLimit(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid() || !Args->HasTypedField<EJson::Number>(TEXT("limit")))
		{
			return AssetFindDefaultLimit;
		}
		const int32 Requested = static_cast<int32>(Args->GetNumberField(TEXT("limit")));
		if (Requested < 1)
		{
			return AssetFindDefaultLimit;
		}
		return FMath::Min(Requested, AssetFindMaxLimit);
	}

	/** Resolve the `recursive` flag (default true — recursive under the
	 *  requested path, matching the registry's expected behaviour for a
	 *  folder-scoped find). */
	bool ResolveRecursive(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid() || !Args->HasTypedField<EJson::Boolean>(TEXT("recursive")))
		{
			return true;
		}
		return Args->GetBoolField(TEXT("recursive"));
	}

	/**
	 * Validate a `class_path` argument ("/Script/Module.Class" form). Returns
	 * true and populates OutClassPath when valid; returns false + a structured
	 * reason when malformed.
	 *
	 * A short (dotless) class name fires an engine ensure inside
	 * FTopLevelAssetPath construction, so validate the shape BEFORE building —
	 * turn an expected-miss (typo'd class name) into a clean structured error.
	 *
	 * Read-only behavior reference: Unreal-MCP's class-path validation.
	 */
	bool TryResolveClassPath(const FString& ClassPathRaw, FTopLevelAssetPath& OutClassPath, FString& OutError)
	{
		if (!ClassPathRaw.StartsWith(TEXT("/")) || !ClassPathRaw.Contains(TEXT(".")))
		{
			OutError = FString::Printf(
				TEXT("'%s' is not a valid class path (expected '/Script/Module.Class')."),
				*ClassPathRaw);
			return false;
		}
		const FTopLevelAssetPath Parsed(ClassPathRaw);
		if (!Parsed.IsValid())
		{
			OutError = FString::Printf(
				TEXT("'%s' is not a valid class path (expected '/Script/Module.Class')."),
				*ClassPathRaw);
			return false;
		}
		OutClassPath = Parsed;
		return true;
	}

	/**
	 * Read a nested field by dot-separated path from a JsonObject. Returns the
	 * located value (object or leaf) or null when any segment is absent or not
	 * an object. Used by ProjectJsonObject to resolve each `paths` branch.
	 */
	TSharedPtr<FJsonValue> ReadNestedField(const TSharedRef<FJsonObject>& Source, const FString& DottedPath)
	{
		// Split on '.' — every segment but the last must resolve to an object;
		// the last segment's value is the branch we project.
		TArray<FString> Segments;
		DottedPath.ParseIntoArray(Segments, TEXT("."));
		if (Segments.Num() == 0)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Current = Source;
		for (int32 i = 0; i < Segments.Num(); ++i)
		{
			const FString& Segment = Segments[i];
			if (!Current.IsValid() || !Current->HasField(Segment))
			{
				return nullptr;
			}
			if (i == Segments.Num() - 1)
			{
				return Current->GetField(Segment);
			}
			// Intermediate segment must be an object to keep walking.
			const TSharedPtr<FJsonValue> Next = Current->GetField(Segment);
			if (!Next.IsValid() || Next->Type != EJson::Object)
			{
				return nullptr;
			}
			Current = Next->AsObject();
		}
		return nullptr;
	}

	/**
	 * Project a JsonObject to only the requested dot-separated branches.
	 * Mirrors the level family's compact projection contract: an absent branch
	 * is dropped silently; a present branch is emitted at its full dotted path
	 * so an agent can re-assemble the structure. Token savings come from
	 * omitting everything the caller did not name.
	 *
	 * P4.1 ships this as the field-projection helper for asset_get_data; later
	 * tools may reuse it for their own scoped reads.
	 */
	TSharedRef<FJsonObject> ProjectJsonObject(const TSharedRef<FJsonObject>& Source, const TArray<FString>& Paths)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		for (const FString& Dotted : Paths)
		{
			const TSharedPtr<FJsonValue> Branch = ReadNestedField(Source, Dotted);
			if (!Branch.IsValid())
			{
				continue;
			}
			// Re-create the dotted path on the output object so the caller
			// sees the same nested shape they asked for.
			TArray<FString> Segments;
			Dotted.ParseIntoArray(Segments, TEXT("."));
			if (Segments.Num() == 0)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Current = Out;
			for (int32 i = 0; i < Segments.Num(); ++i)
			{
				if (i == Segments.Num() - 1)
				{
					Current->SetField(Segments[i], Branch);
				}
				else
				{
					if (!Current->HasTypedField<EJson::Object>(Segments[i]))
					{
						// SetField replaces; build an empty object so the next
						// iteration can descend into it.
						Current->SetObjectField(Segments[i], MakeShared<FJsonObject>());
					}
					Current = Current->GetObjectField(Segments[i]);
				}
			}
		}
		return Out;
	}
}

void FUnrealOpenMcpAssetTools::Register(FUnrealOpenMcpToolRegistry& Registry)
{
	// unreal_open_mcp_asset_find — filtered AssetRegistry query with stable
	// ordering + offset/limit pagination. Filters: `name` (case-insensitive
	// substring), `class_path` ("/Script/Module.Class", subclasses included),
	// `path` (package-path scope), `tag_key`/`tag_value` (registry tag match).
	// Empty filter defaults to /Game (recursive) so a no-arg find never
	// materializes the ENTIRE registry (incl. /Engine).
	//
	// Read-only — no gate, no paths_hint surface. Structured errors:
	//   - invalid_parameter — malformed body
	//   - missing_parameter — `tag_value` set without `tag_key`
	//   - invalid_class_path — class_path not "/Script/Module.Class" form
	Registry.Register(
		TEXT("unreal_open_mcp_asset_find"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString NameFilter = Args->HasTypedField<EJson::String>(TEXT("name"))
				? Args->GetStringField(TEXT("name"))
				: FString();
			const FString ClassPathRaw = Args->HasTypedField<EJson::String>(TEXT("class_path"))
				? Args->GetStringField(TEXT("class_path"))
				: FString();
			const FString PackagePath = Args->HasTypedField<EJson::String>(TEXT("path"))
				? Args->GetStringField(TEXT("path"))
				: FString();
			const FString TagKey = Args->HasTypedField<EJson::String>(TEXT("tag_key"))
				? Args->GetStringField(TEXT("tag_key"))
				: FString();
			const FString TagValue = Args->HasTypedField<EJson::String>(TEXT("tag_value"))
				? Args->GetStringField(TEXT("tag_value"))
				: FString();
			const bool bRecursive = ResolveRecursive(Args);
			const int32 Offset = ResolveOffset(Args);
			const int32 Limit = ResolveLimit(Args);

			// A bare `tag_value` with no `tag_key` is a no-op in the registry
			// filter; surface it as a clear error rather than silently ignoring
			// the caller's intent.
			if (TagKey.IsEmpty() && !TagValue.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'tag_value' requires 'tag_key'."));
			}

			FARFilter Filter;
			if (!PackagePath.IsEmpty())
			{
				Filter.PackagePaths.Add(FName(*PackagePath));
				Filter.bRecursivePaths = bRecursive;
			}
			if (!ClassPathRaw.IsEmpty())
			{
				FTopLevelAssetPath ClassPath;
				FString ClassError;
				if (!TryResolveClassPath(ClassPathRaw, ClassPath, ClassError))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_class_path"),
						ClassError);
				}
				Filter.ClassPaths.Add(ClassPath);
				Filter.bRecursiveClasses = true;
			}
			if (!TagKey.IsEmpty())
			{
				// Value-less tag filter matches any value for that key; a
				// value constrains the match. TOptional<FString>() = match any.
				Filter.TagsAndValues.Add(
					FName(*TagKey),
					TagValue.IsEmpty() ? TOptional<FString>() : TOptional<FString>(TagValue));
			}

			// With no path/class/tag the filter would be empty, forcing
			// GetAssets to materialize the ENTIRE registry (incl. /Engine).
			// Default the scope to /Game instead — a name-only query is still
			// served, but bounded to project content.
			if (Filter.IsEmpty())
			{
				Filter.PackagePaths.Add(FName(TEXT("/Game")));
				Filter.bRecursivePaths = bRecursive;
			}

			TArray<FAssetData> Assets;
			IAssetRegistry& AssetRegistry = GetAssetRegistry();
			AssetRegistry.GetAssets(Filter, Assets);

			// Post-filter by name substring — the registry has no substring
			// filter primitive, so a `name` arg narrows the result client-side.
			if (!NameFilter.IsEmpty())
			{
				Assets.RemoveAll([&NameFilter](const FAssetData& Data)
				{
					return !Data.AssetName.ToString().Contains(NameFilter, ESearchCase::IgnoreCase);
				});
			}

			// Deterministic ordering so pagination is stable across calls.
			// Precompute the object-path sort key ONCE per asset — the
			// comparator runs O(n log n) times, so recomputing the string in
			// the comparator was the hot path on large result sets.
			TArray<TPair<FString, const FAssetData*>> Sortable;
			Sortable.Reserve(Assets.Num());
			for (const FAssetData& Data : Assets)
			{
				Sortable.Emplace(Data.GetObjectPathString(), &Data);
			}
			Sortable.Sort([](const TPair<FString, const FAssetData*>& A, const TPair<FString, const FAssetData*>& B)
			{
				return A.Key < B.Key;
			});

			const int32 Total = Sortable.Num();
			TArray<TSharedPtr<FJsonValue>> Page;
			const int32 WindowEnd = FMath::Min(Offset + Limit, Total);
			for (int32 Index = Offset; Index < WindowEnd; ++Index)
			{
				Page.Add(MakeShared<FJsonValueObject>(AssetDataToJson(*Sortable[Index].Value)));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("total"), Total);
			Result->SetNumberField(TEXT("offset"), Offset);
			Result->SetNumberField(TEXT("count"), Page.Num());
			Result->SetArrayField(TEXT("assets"), Page);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	// unreal_open_mcp_asset_get_data — single-asset metadata read by path-or-
	// name. Returns the AssetSummary block ({ name, path, package, class })
	// plus the asset's registry tag map. `path` accepts either an object path
	// ("/Game/Mat/M_Foo.M_Foo") or a package path ("/Game/Mat/M_Foo"); both are
	// normalized before the registry probe.
	//
	// Optional `paths` string[] projects the result to those dot-separated
	// branches (token savings). P4.1 ships the field-projection contract:
	// when `paths` is present, only the requested branches are emitted; when
	// absent, the full payload is returned. The projection is implemented as
	// a post-serialization filter (same pattern as the level_get_data profile
	// — keep the field list cheap and stateless).
	//
	// Read-only — no gate, no paths_hint surface. Structured errors:
	//   - missing_parameter — `path` absent/empty
	//   - asset_not_found   — no asset exists at the path
	//   - invalid_parameter — malformed body / bad `paths` shape
	Registry.Register(
		TEXT("unreal_open_mcp_asset_get_data"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString PathRaw = Args->HasTypedField<EJson::String>(TEXT("path"))
				? Args->GetStringField(TEXT("path"))
				: FString();
			if (PathRaw.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'path' is required (asset path-or-name, e.g. '/Game/Mat/M_Foo' or '/Game/Mat/M_Foo.M_Foo')."));
			}

			// Resolve `paths` (scoped-read projection) up front so a bad shape
			// fails fast before the registry probe. Non-string entries are
			// dropped; a present-but-not-array `paths` is an error.
			TArray<FString> ScopedPaths;
			if (Args->HasTypedField<EJson::Array>(TEXT("paths")))
			{
				const TArray<TSharedPtr<FJsonValue>>* RawPaths = nullptr;
				if (Args->TryGetArrayField(TEXT("paths"), RawPaths))
				{
					for (const TSharedPtr<FJsonValue>& Entry : *RawPaths)
					{
						if (Entry.IsValid() && Entry->Type == EJson::String)
						{
							const FString Str = Entry->AsString();
							if (!Str.IsEmpty())
							{
								ScopedPaths.Add(Str);
							}
						}
					}
				}
			}
			else if (Args->HasField(TEXT("paths")))
			{
				// `paths` is present but not an array — refuse rather than
				// silently widening the read.
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("'paths' must be an array of dot-separated field names."));
			}

			// DoesAssetExist accepts both object-path and package-path forms;
			// no need to normalize for the existence probe. FindAssetData
			// resolves to the FAssetData the same way.
			if (!UEditorAssetLibrary::DoesAssetExist(PathRaw))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("asset_not_found"),
					FString::Printf(
						TEXT("No asset exists at '%s'."),
						*PathRaw));
			}

			const FAssetData Data = UEditorAssetLibrary::FindAssetData(PathRaw);
			if (!Data.IsValid())
			{
				// DoesAssetExist returned true but FindAssetData missed — the
				// registry state shifted under us (rare, but possible during a
				// background scan). Surface as asset_not_found so the agent
				// retries instead of receiving an empty payload.
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("asset_not_found"),
					FString::Printf(
						TEXT("Asset '%s' vanished between the existence probe and the metadata read."),
						*PathRaw));
			}

			TSharedRef<FJsonObject> Full = AssetDataToJson(Data);

			// Tag map — registry metadata tags (key → string value). The
			// AssetData tag map is the structured surface agents branch on
			// (e.g. FILTER_BY_TYPE, parent class hints). Empty when the asset
			// has no tags.
			TSharedRef<FJsonObject> Tags = MakeShared<FJsonObject>();
			Data.TagsAndValues.ForEach([&Tags](const TPair<FName, FAssetTagValueRef>& Tag)
			{
				Tags->SetStringField(Tag.Key.ToString(), Tag.Value.AsString());
			});
			Full->SetObjectField(TEXT("tags"), Tags);

			// Field projection — when `paths` is supplied, emit only the
			// requested branches. A projection that names an absent branch is
			// dropped silently (matches the level family's compact projection
			// contract). An empty ScopedPaths list (the common case)
			// returns the full payload.
			const TSharedRef<FJsonObject> Result = ScopedPaths.Num() > 0
				? ProjectJsonObject(Full, ScopedPaths)
				: Full;

			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	// =========================================================================
	// P4.2 — Content Browser mutators (create_folder / copy / move / delete)
	// =========================================================================
	//
	// The four mutators share the same discipline:
	//   1. Validate args (path/source/destination present).
	//   2. Refuse engine content roots via IsWritableContentRoot.
	//   3. Resolve destination shape via SplitObjectPath (copy/move only).
	//   4. Refuse destination-already-exists collisions with a structured
	//      error (no silent overwrite; data-loss prevention).
	//   5. Call the EditorAssetLibrary op (MakeDirectory / DuplicateAsset /
	//      RenameAsset / DeleteAsset) and surface the result.
	//
	// delete adds an extra referencer guard BEFORE step 5: the registry is
	// queried for inbound referencers and the call refuses with a structured
	// `delete_blocked_by_referencers` error listing them, unless `force: true`
	// is passed. This is the P4.2 contract for "delete surfaces referencer
	// information (or structured refuse) before breaking soft refs silently"
	// from the plan.
	//
	// All four register with FUnrealOpenMcpToolMetadata::Mutating() so the
	// dispatcher wraps them in GatePolicy.Execute (checkpoint → mutate →
	// validate → delta). The gate's `paths_hint` (mandatory for mutating
	// dispatches) is enforced by the dispatcher BEFORE the handler runs, so
	// the handlers themselves do not re-check it.
	//
	// Structured errors per the P4.2 contract:
	//   - missing_parameter          — required arg absent/empty
	//   - invalid_parameter          — malformed body / bad shape
	//   - invalid_content_root       — write under /Engine, /Script, /Temp
	//   - invalid_path               — destination is not a valid package path
	//   - asset_not_found            — source/path does not exist
	//   - asset_already_exists       — destination collision (copy/move)
	//   - delete_blocked_by_referencers — asset has inbound referencers
	//                                     (force:false); referencer list surfaced
	//   - execution_error            — the editor op returned false
	// =========================================================================

	// unreal_open_mcp_asset_create_folder — MakeDirectory. Idempotent: returns
	// `created: false` when the folder already exists (NOT an error). The
	// writable-root guard refuses /Engine, /Script, /Temp so an agent cannot
	// accidentally scribble into engine content.
	//
	// Mutating (creates a Content Browser folder on disk). Structured errors:
	//   - missing_parameter    — `path` absent/empty
	//   - invalid_content_root — path under /Engine, /Script, /Temp
	//   - invalid_parameter    — malformed body
	//   - execution_error      — MakeDirectory returned false
	Registry.Register(
		TEXT("unreal_open_mcp_asset_create_folder"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString Path = Args->HasTypedField<EJson::String>(TEXT("path"))
				? Args->GetStringField(TEXT("path"))
				: FString();
			if (Path.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'path' is required (package path of the folder, e.g. '/Game/MyFolder')."));
			}

			if (!IsWritableContentRoot(Path))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_content_root"),
					FString::Printf(
						TEXT("Refusing to create '%s' under an engine content root; use a project root like '/Game'."),
						*Path));
			}

			// Idempotent — DoesDirectoryExist is the cheap probe that turns the
			// already-exists case into a structured `created: false` result
			// instead of letting MakeDirectory log an Error.
			if (UEditorAssetLibrary::DoesDirectoryExist(Path))
			{
				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("path"), Path);
				Result->SetBoolField(TEXT("created"), false);
				return FUnrealOpenMcpToolDispatchResult::Ok(
					WriteJson(MakeShared<FJsonValueObject>(Result)));
			}

			if (!UEditorAssetLibrary::MakeDirectory(Path))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("execution_error"),
					FString::Printf(TEXT("Failed to create folder '%s'."), *Path));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("path"), Path);
			Result->SetBoolField(TEXT("created"), true);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// unreal_open_mcp_asset_copy — DuplicateAsset. `source` is the existing
	// asset to copy (path-or-name); `destination` is the new package path. The
	// destination must NOT already exist — a collision is a structured error
	// (no silent overwrite, no data loss). Intermediate destination folders
	// must already exist (call asset_create_folder first).
	//
	// Mutating (writes a new asset on disk). Structured errors:
	//   - missing_parameter     — `source`/`destination` absent
	//   - asset_not_found       — source does not exist
	//   - invalid_path          — destination is not a valid package path
	//   - invalid_content_root  — destination under /Engine, /Script, /Temp
	//   - asset_already_exists  — destination already exists (collision)
	//   - execution_error       — DuplicateAsset returned false
	Registry.Register(
		TEXT("unreal_open_mcp_asset_copy"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString Source = Args->HasTypedField<EJson::String>(TEXT("source"))
				? Args->GetStringField(TEXT("source"))
				: FString();
			const FString Destination = Args->HasTypedField<EJson::String>(TEXT("destination"))
				? Args->GetStringField(TEXT("destination"))
				: FString();
			if (Source.IsEmpty() || Destination.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'source' and 'destination' are required (path-or-name of the asset to copy and the new package path)."));
			}

			if (!UEditorAssetLibrary::DoesAssetExist(Source))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("asset_not_found"),
					FString::Printf(TEXT("No source asset at '%s'."), *Source));
			}

			// Validate destination shape up front so a malformed path returns
			// invalid_path instead of a logged engine Error + generic failure.
			FString DestPackagePath;
			FString DestAssetName;
			if (!SplitObjectPath(Destination, DestPackagePath, DestAssetName))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_path"),
					FString::Printf(
						TEXT("'%s' is not a valid destination package path (expected '/Game/Folder/AssetName')."),
						*Destination));
			}

			if (!IsWritableContentRoot(Destination))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_content_root"),
					FString::Printf(
						TEXT("Refusing to copy into '%s' under an engine content root; use a project root like '/Game'."),
						*Destination));
			}

			if (UEditorAssetLibrary::DoesAssetExist(Destination))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("asset_already_exists"),
					FString::Printf(TEXT("Destination '%s' already exists."), *Destination));
			}

			if (!UEditorAssetLibrary::DuplicateAsset(Source, Destination))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("execution_error"),
					FString::Printf(TEXT("Failed to copy '%s' -> '%s'."), *Source, *Destination));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("source"), Source);
			Result->SetStringField(TEXT("destination"), Destination);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// unreal_open_mcp_asset_move — RenameAsset. `source` is the existing asset
	// to move/rename; `destination` is the new package path. Destination must
	// NOT already exist. A redirector MAY remain at the source path — the
	// structured result surfaces this as a `note` field so the agent knows to
	// fix up references separately.
	//
	// Mutating (renames a package on disk). Structured errors mirror copy:
	//   - missing_parameter / asset_not_found / invalid_path
	//   - invalid_content_root / asset_already_exists
	//   - execution_error       — RenameAsset returned false
	Registry.Register(
		TEXT("unreal_open_mcp_asset_move"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString Source = Args->HasTypedField<EJson::String>(TEXT("source"))
				? Args->GetStringField(TEXT("source"))
				: FString();
			const FString Destination = Args->HasTypedField<EJson::String>(TEXT("destination"))
				? Args->GetStringField(TEXT("destination"))
				: FString();
			if (Source.IsEmpty() || Destination.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'source' and 'destination' are required (path-or-name of the asset to move and the new package path)."));
			}

			if (!UEditorAssetLibrary::DoesAssetExist(Source))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("asset_not_found"),
					FString::Printf(TEXT("No source asset at '%s'."), *Source));
			}

			FString DestPackagePath;
			FString DestAssetName;
			if (!SplitObjectPath(Destination, DestPackagePath, DestAssetName))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_path"),
					FString::Printf(
						TEXT("'%s' is not a valid destination package path (expected '/Game/Folder/AssetName')."),
						*Destination));
			}

			if (!IsWritableContentRoot(Destination))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_content_root"),
					FString::Printf(
						TEXT("Refusing to move into '%s' under an engine content root; use a project root like '/Game'."),
						*Destination));
			}

			if (UEditorAssetLibrary::DoesAssetExist(Destination))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("asset_already_exists"),
					FString::Printf(TEXT("Destination '%s' already exists."), *Destination));
			}

			if (!UEditorAssetLibrary::RenameAsset(Source, Destination))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("execution_error"),
					FString::Printf(TEXT("Failed to move '%s' -> '%s'."), *Source, *Destination));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("source"), Source);
			Result->SetStringField(TEXT("destination"), Destination);
			// A redirector may remain at the source path — surface as a note
			// so the agent knows references to the old path may need fixing.
			Result->SetStringField(TEXT("note"), TEXT("A redirector may remain at the source path."));
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// unreal_open_mcp_asset_delete — DeleteAsset with a referencer guard.
	// `path` is the asset to delete; `force` (default false) overrides the
	// referencer guard. Without `force`, the registry is queried for inbound
	// referencers and the call REFUSES with `delete_blocked_by_referencers`,
	// surfacing the (bounded) referencer list so the agent can decide.
	//
	// This is the P4.2 contract: "Delete surfaces referencer information (or
	// structured refuse) before breaking soft refs silently". The gate's
	// broken_soft_references rule still runs in the validate pass after a
	// forced delete; this guard is the BEFORE hook that prevents an agent
	// from learning about broken refs only after the fact.
	//
	// Mutating (deletes a package on disk; not undoable from MCP). Errors:
	//   - missing_parameter / asset_not_found / invalid_content_root
	//   - delete_blocked_by_referencers — asset has inbound referencers
	//                                     (force:false); referencer list in message
	//   - execution_error       — DeleteAsset returned false
	Registry.Register(
		TEXT("unreal_open_mcp_asset_delete"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString Path = Args->HasTypedField<EJson::String>(TEXT("path"))
				? Args->GetStringField(TEXT("path"))
				: FString();
			if (Path.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'path' is required (path-or-name of the asset to delete)."));
			}

			const bool bForce = Args->HasTypedField<EJson::Boolean>(TEXT("force"))
				&& Args->GetBoolField(TEXT("force"));

			if (!UEditorAssetLibrary::DoesAssetExist(Path))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("asset_not_found"),
					FString::Printf(TEXT("No asset at '%s'."), *Path));
			}

			// Engine-root guard — this is the family's most destructive write,
			// so it refuses /Engine, /Script, /Temp like create-folder/copy/
			// move do (without it, force:true on '/Engine/...' would
			// DeleteAsset engine content).
			if (!IsWritableContentRoot(Path))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_content_root"),
					FString::Printf(
						TEXT("Refusing to delete '%s' under an engine content root; use a project root like '/Game'."),
						*Path));
			}

			// Referencer guard — UEditorAssetLibrary::DeleteAsset force-deletes
			// WITHOUT confirmation, silently dangling inbound references.
			// Query the registry for on-disk referencers first and refuse
			// (unless `force`) with the referencer list so the caller can decide.
			if (!bForce)
			{
				const FAssetData Data = UEditorAssetLibrary::FindAssetData(Path);
				if (Data.IsValid() && !Data.PackageName.IsNone())
				{
					TArray<FName> Referencers;
					GetAssetRegistry().GetReferencers(Data.PackageName, Referencers);
					// The asset's own package is sometimes self-referenced
					// (e.g. a material referencing its own texture parameter);
					// never count the asset itself.
					Referencers.Remove(Data.PackageName);
					if (Referencers.Num() > 0)
					{
						// Bound the list — the first 10 referencers + a "+N more"
						// counter so the message stays readable on large fan-ins.
						// The envelope ships {code, message} verbatim on Fail, so
						// the structured referencer list rides inside the message
						// itself: agents read the count in the prefix and the
						// bounded list in brackets, and chain into
						// unreal_open_mcp_find_references for the full closure.
						FString List;
						const int32 Shown = FMath::Min(Referencers.Num(), 10);
						for (int32 Index = 0; Index < Shown; ++Index)
						{
							if (Index > 0)
							{
								List += TEXT(", ");
							}
							List += Referencers[Index].ToString();
						}
						if (Referencers.Num() > Shown)
						{
							List += FString::Printf(TEXT(", (+%d more)"), Referencers.Num() - Shown);
						}
						return FUnrealOpenMcpToolDispatchResult::Fail(
							TEXT("delete_blocked_by_referencers"),
							FString::Printf(
								TEXT("Refusing to delete '%s': referenced by %d package(s) [%s]. Pass 'force':true to delete anyway (the gate's broken_soft_references rule will still flag any dangling refs)."),
								*Path, Referencers.Num(), *List));
					}
				}
			}

			if (!UEditorAssetLibrary::DeleteAsset(Path))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("execution_error"),
					FString::Printf(TEXT("Failed to delete '%s'."), *Path));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("path"), Path);
			Result->SetBoolField(TEXT("deleted"), true);
			Result->SetBoolField(TEXT("forced"), bForce);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// unreal_open_mcp_asset_refresh — IAssetRegistry::ScanPathsSynchronous.
	// Rescans the requested package paths so newly added files on disk become
	// visible to the registry. `paths` (array) defaults to ['/Game'] when
	// omitted; a single-string `path` is also accepted for ergonomics.
	// `force: true` triggers a full blocking rescan (slow for large trees).
	//
	// Classification: READ-ONLY. Unlike Unity's AssetDatabase.Refresh (which
	// can trigger an import/compile, hence Unity marks assets_refresh
	// mutating), Unreal's ScanPathsSynchronous only updates the in-memory
	// AssetRegistry cache — it does not write packages or change the UObject
	// graph. The gate would have no on-disk diff to checkpoint. Registered
	// with the ReadOnly() metadata so the dispatcher runs it directly.
	//
	// Structured errors:
	//   - invalid_parameter — malformed body / bad `paths` shape
	// Result: { paths: string[], force: boolean }
	Registry.Register(
		TEXT("unreal_open_mcp_asset_refresh"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			TArray<FString> Paths;
			if (Args->HasTypedField<EJson::Array>(TEXT("paths")))
			{
				const TArray<TSharedPtr<FJsonValue>>* RawPaths = nullptr;
				if (Args->TryGetArrayField(TEXT("paths"), RawPaths))
				{
					for (const TSharedPtr<FJsonValue>& Entry : *RawPaths)
					{
						if (Entry.IsValid() && Entry->Type == EJson::String)
						{
							const FString Str = Entry->AsString();
							if (!Str.IsEmpty())
							{
								Paths.AddUnique(Str);
							}
						}
					}
				}
			}
			else if (Args->HasField(TEXT("paths")))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("'paths' must be an array of package paths."));
			}

			// Ergonomic single-path alternative — appended to `paths` if both
			// are supplied (deduped by AddUnique).
			if (Args->HasTypedField<EJson::String>(TEXT("path")))
			{
				const FString SinglePath = Args->GetStringField(TEXT("path"));
				if (!SinglePath.IsEmpty())
				{
					Paths.AddUnique(SinglePath);
				}
			}

			const bool bForce = Args->HasTypedField<EJson::Boolean>(TEXT("force"))
				&& Args->GetBoolField(TEXT("force"));

			if (Paths.Num() == 0)
			{
				// Empty filter → /Game (same default as asset_find; never the
				// whole registry incl. /Engine by accident).
				Paths.Add(TEXT("/Game"));
			}

			GetAssetRegistry().ScanPathsSynchronous(Paths, /*bForceRescan*/ bForce);

			TArray<TSharedPtr<FJsonValue>> ScannedValues;
			ScannedValues.Reserve(Paths.Num());
			for (const FString& P : Paths)
			{
				ScannedValues.Add(MakeShared<FJsonValueString>(P));
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("paths"), ScannedValues);
			Result->SetBoolField(TEXT("force"), bForce);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	UE_LOG(
		LogUnrealOpenMcp,
		Log,
		TEXT("[Unreal Open MCP] asset tools registered: unreal_open_mcp_asset_find, unreal_open_mcp_asset_get_data, unreal_open_mcp_asset_create_folder, unreal_open_mcp_asset_copy, unreal_open_mcp_asset_move, unreal_open_mcp_asset_delete, unreal_open_mcp_asset_refresh"));
}
