// Material-tool family — see header for the create/modify/get-data contracts.
// This file owns the three handlers (material_create, material_modify,
// material_get_data) plus the small set of path/JSON helpers they need.
//
// Arg parsing + output mirror the asset family: each handler parses the raw
// POST body into an FJsonObject and emits a pre-serialized JSON string handed
// to FUnrealOpenMcpToolDispatchResult::Output. The registry/dispatch spine
// stays raw-body — only the handler layer parses.
//
// The path/JSON helpers here (ParseBody / WriteJson / SplitObjectPath /
// IsWritableContentRoot / ProjectJsonObject) are kept LOCAL to this family
// rather than shared out of the asset family's anonymous namespace — the
// Unreal-MCP reference keeps each family's helpers local, and duplicating a
// handful of tiny pure functions is cheaper than widening a public header for
// two translation units. Consolidation into a shared header can happen later.
//
// Behavior reference: Unreal-MCP's asset-material-create / asset-material-modify
// / asset-material-get-data (UnrealMcpAssetTools.cpp). The factory GC-root
// guard, the "engine setters always return false → validate names against the
// known parameter set" applied/failed classification, the partial-vector
// seed-from-current, the empty-modify transaction cancel (skip MarkPackageDirty),
// and the base-material default value fallback were studied for correct Unreal
// editor API usage and adapted to this port's Ok/Fail result shape.
#include "Tools/UnrealOpenMcpMaterialTools.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "UnrealOpenMcpLog.h"

// EditorAssetLibrary owns the path-or-name existence probe + load primitives
// (DoesAssetExist / LoadAsset / SaveLoadedAsset) the family leans on.
#include "EditorAssetLibrary.h"
// AssetTools owns IAssetTools::CreateAsset, the factory-driven create path.
#include "AssetToolsModule.h"
#include "IAssetTools.h"
// The MIC factory (create from a parent material interface).
#include "Factories/MaterialInstanceConstantFactoryNew.h"
// UMaterialEditingLibrary is the editor-only parameter read/write surface
// (GetScalarParameterNames / SetMaterialInstance*ParameterValue / defaults).
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/Texture.h"
// FGCObjectScopeGuard roots the transient factory across CreateAsset so a GC
// pass mid-create cannot collect it.
#include "UObject/GCObjectScopeGuard.h"
// FScopedTransaction wraps the parameter writes so an editor Ctrl+Z can undo
// an MCP-driven material edit (matches editor-native behaviour).
#include "ScopedTransaction.h"

#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "UnrealOpenMcpMaterialTools"

namespace
{
	/** Parse the raw POST body into a JSON object. Empty body → empty object;
	 *  malformed body → null (caller surfaces `invalid_parameter`). Same
	 *  contract as the asset family's ParseBody. */
	TSharedPtr<FJsonObject> ParseBody(const FString& Body)
	{
		const FString Trimmed = Body.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
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

	/** Serialize a JsonValue to a compact string ("null" on a null pointer so
	 *  the result is always valid JSON). Same shape as the asset family. */
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

	/** Access the running AssetTools module (IAssetTools::CreateAsset). Loaded
	 *  checked — the bridge is an editor module and AssetTools is always present
	 *  in the editor. */
	IAssetTools& GetAssetTools()
	{
		return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	}

	/**
	 * Reject the engine/script/temp content roots for WRITE operations. Mirrors
	 * the asset family's IsWritableContentRoot (kept local; see file header).
	 */
	bool IsWritableContentRoot(const FString& InPath)
	{
		auto IsReservedRoot = [&InPath](const TCHAR* Root)
		{
			return InPath.Equals(Root) || InPath.StartsWith(FString(Root) + TEXT("/"));
		};
		return !(IsReservedRoot(TEXT("/Engine"))
			|| IsReservedRoot(TEXT("/Script"))
			|| IsReservedRoot(TEXT("/Temp")));
	}

	/**
	 * Split a destination path into (package-path, asset-name). Accepts an
	 * object-path ("/Game/Mat/MI_Foo.MI_Foo") or a package-path
	 * ("/Game/Mat/MI_Foo"). Returns false when the path cannot name a valid
	 * destination asset. Mirrors the asset family's SplitObjectPath.
	 */
	bool SplitObjectPath(const FString& InPath, FString& OutPackagePath, FString& OutAssetName)
	{
		FString Work = InPath;
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
		if (!Work.FindLastChar(TEXT('/'), SlashIndex) || SlashIndex == 0 || SlashIndex == Work.Len() - 1)
		{
			return false;
		}
		OutPackagePath = Work.Left(SlashIndex);
		OutAssetName = Work.Mid(SlashIndex + 1);
		return !OutAssetName.IsEmpty() && !OutPackagePath.IsEmpty();
	}

	/** Serialize an FLinearColor into the {r,g,b,a} object shape material_modify
	 *  accepts as vector input — so get-data output round-trips into modify. */
	TSharedRef<FJsonObject> LinearColorToJson(const FLinearColor& Color)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("r"), Color.R);
		Obj->SetNumberField(TEXT("g"), Color.G);
		Obj->SetNumberField(TEXT("b"), Color.B);
		Obj->SetNumberField(TEXT("a"), Color.A);
		return Obj;
	}

	/**
	 * Read a nested field by dot-separated path. Mirrors the asset family's
	 * ReadNestedField (used by ProjectJsonObject for the scoped-read projection).
	 */
	TSharedPtr<FJsonValue> ReadNestedField(const TSharedRef<FJsonObject>& Source, const FString& DottedPath)
	{
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
	 * Project a JsonObject to only the requested dot-separated branches. Mirrors
	 * the asset family's ProjectJsonObject (compact scoped-read contract: an
	 * absent branch is dropped silently; a present branch is emitted at its full
	 * dotted path).
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
						Current->SetObjectField(Segments[i], MakeShared<FJsonObject>());
					}
					Current = Current->GetObjectField(Segments[i]);
				}
			}
		}
		return Out;
	}
}

void FUnrealOpenMcpMaterialTools::Register(FUnrealOpenMcpToolRegistry& Registry)
{
	// =========================================================================
	// unreal_open_mcp_material_create — UMaterialInstanceConstant from a parent.
	// =========================================================================
	//
	// `parent` is the parent material/material-interface (path-or-name);
	// `destination` is the full package path of the new instance (e.g.
	// '/Game/Mat/MI_Foo'). The parent must exist and be a UMaterialInterface;
	// the destination must NOT already exist (no silent overwrite) and must live
	// under a writable content root.
	//
	// Mutating (writes a new asset). The transient factory is GC-rooted across
	// CreateAsset (a GC pass mid-create would otherwise collect it and crash).
	// The gate's mandatory `paths_hint` is enforced by the dispatcher BEFORE the
	// handler runs. Structured errors:
	//   - invalid_parameter    — malformed body
	//   - missing_parameter    — `parent`/`destination` absent
	//   - parent_not_found     — parent asset does not exist
	//   - not_a_material       — parent is not a UMaterialInterface
	//   - invalid_path         — destination is not a valid package path
	//   - invalid_content_root — destination under /Engine, /Script, /Temp
	//   - asset_already_exists — destination already exists (collision)
	//   - execution_error      — CreateAsset returned null
	Registry.Register(
		TEXT("unreal_open_mcp_material_create"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString ParentPath = Args->HasTypedField<EJson::String>(TEXT("parent"))
				? Args->GetStringField(TEXT("parent"))
				: FString();
			const FString Destination = Args->HasTypedField<EJson::String>(TEXT("destination"))
				? Args->GetStringField(TEXT("destination"))
				: FString();
			if (ParentPath.IsEmpty() || Destination.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'parent' and 'destination' are required (parent material path-or-name and the new instance package path)."));
			}

			// Guard before LoadAsset: loading a missing asset logs an engine
			// Error (the Automation harness counts it as a failure).
			// DoesAssetExist is the clean pre-check.
			if (!UEditorAssetLibrary::DoesAssetExist(ParentPath))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("parent_not_found"),
					FString::Printf(TEXT("Parent material '%s' does not exist."), *ParentPath));
			}

			UObject* ParentObject = UEditorAssetLibrary::LoadAsset(ParentPath);
			UMaterialInterface* Parent = Cast<UMaterialInterface>(ParentObject);
			if (!Parent)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("not_a_material"),
					FString::Printf(TEXT("'%s' is not a material / material-interface."), *ParentPath));
			}

			FString PackagePath;
			FString AssetName;
			if (!SplitObjectPath(Destination, PackagePath, AssetName))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_path"),
					FString::Printf(
						TEXT("'%s' is not a valid destination package path (expected '/Game/Folder/MI_Name')."),
						*Destination));
			}

			if (!IsWritableContentRoot(Destination))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_content_root"),
					FString::Printf(
						TEXT("Refusing to create '%s' under an engine content root; use a project root like '/Game'."),
						*Destination));
			}

			if (UEditorAssetLibrary::DoesAssetExist(Destination))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("asset_already_exists"),
					FString::Printf(TEXT("An asset already exists at '%s'."), *Destination));
			}

			UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
			// Root the factory for the duration of CreateAsset — a GC pass mid-
			// create would otherwise collect the unreferenced factory and crash.
			FGCObjectScopeGuard FactoryGuard(Factory);
			Factory->InitialParent = Parent;

			UObject* Created = GetAssetTools().CreateAsset(
				AssetName, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory);
			if (!Created)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("execution_error"),
					FString::Printf(TEXT("Failed to create material instance at '%s'."), *Destination));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("path"), Created->GetPathName());
			Result->SetStringField(TEXT("parent"), Parent->GetPathName());
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// =========================================================================
	// unreal_open_mcp_material_modify — set scalar/vector/texture parameters.
	// =========================================================================
	//
	// `path` is the UMaterialInstanceConstant to edit (path-or-name). At least
	// one of `scalars` (name → number), `vectors` (name → {r,g,b,a}), or
	// `textures` (name → texture path-or-name) MUST be supplied — an empty
	// no-op modify is refused with `nothing_to_modify` (a no-op would needlessly
	// dirty the package + open/close a transaction). `save: true` writes the
	// package to disk after applying; default false leaves it dirty in-memory.
	//
	// The engine's UMaterialEditingLibrary setters ALWAYS return false, so
	// applied-vs-failed is determined by validating each requested name against
	// the instance's known parameter set (NOT the setter return). Unknown names
	// and missing textures land in `failed` rather than aborting the whole call.
	// When NOTHING applies, the transaction is cancelled and the package is left
	// untouched (MarkPackageDirty is NOT undone by Transaction.Cancel, so we
	// must skip it entirely).
	//
	// Mutating. Structured errors:
	//   - invalid_parameter     — malformed body / bad shape
	//   - missing_parameter     — `path` absent
	//   - asset_not_found       — no asset at `path`
	//   - invalid_content_root  — path under /Engine, /Script, /Temp
	//   - not_a_material_instance — path is not a UMaterialInstanceConstant
	//   - nothing_to_modify     — no scalars/vectors/textures supplied, or none
	//                             applied (unknown names / missing textures)
	Registry.Register(
		TEXT("unreal_open_mcp_material_modify"),
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
					TEXT("'path' is required (material-instance path-or-name)."));
			}

			const bool bSave = Args->HasTypedField<EJson::Boolean>(TEXT("save"))
				&& Args->GetBoolField(TEXT("save"));

			// Require at least one parameter object up front — a no-op modify
			// would needlessly dirty the package (and open/close a transaction).
			const TSharedPtr<FJsonObject>* Scalars = nullptr;
			const TSharedPtr<FJsonObject>* Vectors = nullptr;
			const TSharedPtr<FJsonObject>* Textures = nullptr;
			const bool bHasScalars = Args->TryGetObjectField(TEXT("scalars"), Scalars);
			const bool bHasVectors = Args->TryGetObjectField(TEXT("vectors"), Vectors);
			const bool bHasTextures = Args->TryGetObjectField(TEXT("textures"), Textures);
			if (!bHasScalars && !bHasVectors && !bHasTextures)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("nothing_to_modify"),
					TEXT("Provide at least one of 'scalars', 'vectors', or 'textures'."));
			}

			if (!UEditorAssetLibrary::DoesAssetExist(Path))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("asset_not_found"),
					FString::Printf(TEXT("No asset at '%s'."), *Path));
			}

			// Engine-root guard: even save:false dirties the package (save:true
			// persists it), so refuse /Engine, /Script, /Temp consistently.
			if (!IsWritableContentRoot(Path))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_content_root"),
					FString::Printf(
						TEXT("Refusing to modify '%s' under an engine content root; use a project root like '/Game'."),
						*Path));
			}

			UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(UEditorAssetLibrary::LoadAsset(Path));
			if (!Instance)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("not_a_material_instance"),
					FString::Printf(TEXT("'%s' is not a UMaterialInstanceConstant."), *Path));
			}

			// Engine setters always return false → validate names against the
			// instance's known parameter set to classify applied vs failed.
			TArray<FName> ScalarNameList;
			TArray<FName> VectorNameList;
			TArray<FName> TextureNameList;
			UMaterialEditingLibrary::GetScalarParameterNames(Instance, ScalarNameList);
			UMaterialEditingLibrary::GetVectorParameterNames(Instance, VectorNameList);
			UMaterialEditingLibrary::GetTextureParameterNames(Instance, TextureNameList);
			const TSet<FName> KnownScalars(ScalarNameList);
			const TSet<FName> KnownVectors(VectorNameList);
			const TSet<FName> KnownTextures(TextureNameList);

			TArray<FString> AppliedScalars;
			TArray<FString> AppliedVectors;
			TArray<FString> AppliedTextures;
			TArray<FString> Failed;

			// Wrap the writes in a transaction + Modify() so the edit is undoable
			// (Ctrl+Z), matching editor-driven material edits.
			FScopedTransaction Transaction(LOCTEXT("MaterialModify", "Modify Material Instance (MCP)"));
			Instance->Modify();

			if (bHasScalars)
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Scalars)->Values)
				{
					const FName ParamName(*Pair.Key);
					double Value;
					if (!KnownScalars.Contains(ParamName))
					{
						Failed.Add(FString::Printf(TEXT("scalar:%s (unknown parameter)"), *Pair.Key));
					}
					else if (Pair.Value.IsValid() && Pair.Value->TryGetNumber(Value))
					{
						UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, ParamName, static_cast<float>(Value));
						AppliedScalars.Add(Pair.Key);
					}
					else
					{
						Failed.Add(FString::Printf(TEXT("scalar:%s (not a number)"), *Pair.Key));
					}
				}
			}

			if (bHasVectors)
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Vectors)->Values)
				{
					const FName ParamName(*Pair.Key);
					const TSharedPtr<FJsonObject>* ColorObj;
					if (!KnownVectors.Contains(ParamName))
					{
						Failed.Add(FString::Printf(TEXT("vector:%s (unknown parameter)"), *Pair.Key));
					}
					else if (Pair.Value.IsValid() && Pair.Value->TryGetObject(ColorObj))
					{
						// Seed from the CURRENT value so a partial object (e.g.
						// {"r":1}) preserves the other components instead of
						// zeroing them to Black.
						FLinearColor Color = UMaterialEditingLibrary::GetMaterialInstanceVectorParameterValue(Instance, ParamName);
						double Component;
						if ((*ColorObj)->TryGetNumberField(TEXT("r"), Component)) Color.R = static_cast<float>(Component);
						if ((*ColorObj)->TryGetNumberField(TEXT("g"), Component)) Color.G = static_cast<float>(Component);
						if ((*ColorObj)->TryGetNumberField(TEXT("b"), Component)) Color.B = static_cast<float>(Component);
						if ((*ColorObj)->TryGetNumberField(TEXT("a"), Component)) Color.A = static_cast<float>(Component);
						UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, ParamName, Color);
						AppliedVectors.Add(Pair.Key);
					}
					else
					{
						Failed.Add(FString::Printf(TEXT("vector:%s (expected {r,g,b,a} object)"), *Pair.Key));
					}
				}
			}

			if (bHasTextures)
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Textures)->Values)
				{
					const FName ParamName(*Pair.Key);
					FString TexturePath;
					UTexture* Texture = nullptr;
					const bool bGaveTexturePath = Pair.Value.IsValid() && Pair.Value->TryGetString(TexturePath) && !TexturePath.IsEmpty();
					if (bGaveTexturePath && UEditorAssetLibrary::DoesAssetExist(TexturePath))
					{
						Texture = Cast<UTexture>(UEditorAssetLibrary::LoadAsset(TexturePath));
					}
					if (KnownTextures.Contains(ParamName) && Texture)
					{
						UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, ParamName, Texture);
						AppliedTextures.Add(Pair.Key);
					}
					else if (!KnownTextures.Contains(ParamName))
					{
						Failed.Add(FString::Printf(TEXT("texture:%s (unknown parameter)"), *Pair.Key));
					}
					else
					{
						Failed.Add(FString::Printf(TEXT("texture:%s (texture not found: '%s')"), *Pair.Key, *TexturePath));
					}
				}
			}

			const int32 AppliedCount = AppliedScalars.Num() + AppliedVectors.Num() + AppliedTextures.Num();

			// Nothing applied → cancel the transaction AND skip
			// UpdateMaterialInstance/MarkPackageDirty (MarkPackageDirty is NOT
			// undone by Transaction.Cancel, so dirtying here would leave a no-op
			// edit). Fires for both the all-failed case and the all-empty case
			// (every supplied parameter object was empty, e.g. {"scalars":{}}).
			if (AppliedCount == 0)
			{
				Transaction.Cancel();
				const FString Reason = Failed.Num() > 0
					? FString::Printf(TEXT("%d failed — unknown parameter names or missing textures"), Failed.Num())
					: TEXT("no applicable parameters supplied");
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("nothing_to_modify"),
					FString::Printf(TEXT("No parameters applied to '%s' (%s)."), *Instance->GetName(), *Reason));
			}

			// At least one parameter applied → commit + dirty the package.
			UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
			Instance->MarkPackageDirty();

			bool bSaved = false;
			if (bSave)
			{
				bSaved = UEditorAssetLibrary::SaveLoadedAsset(Instance, /*bOnlyIfIsDirty*/ false);
			}

			auto ToJsonArray = [](const TArray<FString>& Items)
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				Values.Reserve(Items.Num());
				for (const FString& Item : Items)
				{
					Values.Add(MakeShared<FJsonValueString>(Item));
				}
				return Values;
			};

			TSharedRef<FJsonObject> Applied = MakeShared<FJsonObject>();
			Applied->SetArrayField(TEXT("scalars"), ToJsonArray(AppliedScalars));
			Applied->SetArrayField(TEXT("vectors"), ToJsonArray(AppliedVectors));
			Applied->SetArrayField(TEXT("textures"), ToJsonArray(AppliedTextures));

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("path"), Instance->GetPathName());
			Result->SetObjectField(TEXT("applied"), Applied);
			Result->SetArrayField(TEXT("failed"), ToJsonArray(Failed));
			Result->SetBoolField(TEXT("saved"), bSaved);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// =========================================================================
	// unreal_open_mcp_material_get_data — parameter inventory + current values.
	// =========================================================================
	//
	// `path` is a material or material-instance (path-or-name). Returns the
	// scalar / vector / texture parameter names + current values — the instance
	// override for a UMaterialInstanceConstant, or the base-material default for
	// a UMaterial — plus the parent path (instances only) and an `isInstance`
	// flag. The scalars/vectors/textures shapes match material_modify's input so
	// get-data output chains directly into a modify call.
	//
	// Optional `paths` (dot-separated field names) projects the result to those
	// branches only (token savings) — same contract as asset_get_data.
	//
	// Read-only — no gate, no paths_hint surface. Structured errors:
	//   - invalid_parameter — malformed body / non-array `paths`
	//   - missing_parameter — `path` absent
	//   - asset_not_found   — no asset at `path`
	//   - not_a_material    — path is not a material / material-interface
	Registry.Register(
		TEXT("unreal_open_mcp_material_get_data"),
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
					TEXT("'path' is required (material or material-instance path-or-name)."));
			}

			// Resolve `paths` (scoped-read projection) up front so a bad shape
			// fails fast before the load. Non-string entries are dropped; a
			// present-but-not-array `paths` is an error.
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
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("'paths' must be an array of dot-separated field names."));
			}

			if (!UEditorAssetLibrary::DoesAssetExist(Path))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("asset_not_found"),
					FString::Printf(TEXT("No asset at '%s'."), *Path));
			}

			UMaterialInterface* Material = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(Path));
			if (!Material)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("not_a_material"),
					FString::Printf(TEXT("'%s' is not a material / material-interface."), *Path));
			}

			UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Material);
			// A base material still reports each parameter's DEFAULT value via
			// the GetMaterialDefault* family (rather than null).
			UMaterial* BaseMaterial = Instance ? nullptr : Cast<UMaterial>(Material);

			TArray<FName> ScalarNames;
			TArray<FName> VectorNames;
			TArray<FName> TextureNames;
			UMaterialEditingLibrary::GetScalarParameterNames(Material, ScalarNames);
			UMaterialEditingLibrary::GetVectorParameterNames(Material, VectorNames);
			UMaterialEditingLibrary::GetTextureParameterNames(Material, TextureNames);

			TSharedRef<FJsonObject> Scalars = MakeShared<FJsonObject>();
			for (const FName& Name : ScalarNames)
			{
				if (Instance)
				{
					Scalars->SetNumberField(Name.ToString(), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, Name));
				}
				else if (BaseMaterial)
				{
					Scalars->SetNumberField(Name.ToString(), UMaterialEditingLibrary::GetMaterialDefaultScalarParameterValue(BaseMaterial, Name));
				}
				else
				{
					Scalars->SetField(Name.ToString(), MakeShared<FJsonValueNull>());
				}
			}

			TSharedRef<FJsonObject> Vectors = MakeShared<FJsonObject>();
			for (const FName& Name : VectorNames)
			{
				if (Instance)
				{
					Vectors->SetObjectField(Name.ToString(), LinearColorToJson(UMaterialEditingLibrary::GetMaterialInstanceVectorParameterValue(Instance, Name)));
				}
				else if (BaseMaterial)
				{
					Vectors->SetObjectField(Name.ToString(), LinearColorToJson(UMaterialEditingLibrary::GetMaterialDefaultVectorParameterValue(BaseMaterial, Name)));
				}
				else
				{
					Vectors->SetField(Name.ToString(), MakeShared<FJsonValueNull>());
				}
			}

			TSharedRef<FJsonObject> Textures = MakeShared<FJsonObject>();
			for (const FName& Name : TextureNames)
			{
				UTexture* Texture = nullptr;
				if (Instance)
				{
					Texture = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, Name);
				}
				else if (BaseMaterial)
				{
					Texture = UMaterialEditingLibrary::GetMaterialDefaultTextureParameterValue(BaseMaterial, Name);
				}
				if (Texture)
				{
					Textures->SetStringField(Name.ToString(), Texture->GetPathName());
				}
				else
				{
					Textures->SetField(Name.ToString(), MakeShared<FJsonValueNull>());
				}
			}

			TSharedRef<FJsonObject> Full = MakeShared<FJsonObject>();
			Full->SetStringField(TEXT("path"), Material->GetPathName());
			Full->SetBoolField(TEXT("isInstance"), Instance != nullptr);
			if (Instance && Instance->Parent)
			{
				Full->SetStringField(TEXT("parent"), Instance->Parent->GetPathName());
			}
			Full->SetObjectField(TEXT("scalars"), Scalars);
			Full->SetObjectField(TEXT("vectors"), Vectors);
			Full->SetObjectField(TEXT("textures"), Textures);

			const TSharedRef<FJsonObject> Result = ScopedPaths.Num() > 0
				? ProjectJsonObject(Full, ScopedPaths)
				: Full;

			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	UE_LOG(
		LogUnrealOpenMcp,
		Log,
		TEXT("[Unreal Open MCP] material tools registered: unreal_open_mcp_material_create, unreal_open_mcp_material_modify, unreal_open_mcp_material_get_data"));
}

#undef LOCTEXT_NAMESPACE
