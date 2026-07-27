// Blueprint-tool family — see header for the create/get/component/variable/
// function/event/compile/spawn contracts. This file owns the handlers
// (blueprint_create, blueprint_get, blueprint_add/remove_component,
// blueprint_add/modify_variable, blueprint_set_default,
// blueprint_add_function, blueprint_add_event, blueprint_compile,
// blueprint_spawn) plus the shared helpers the later P6 sub-plans reuse
// (ResolveBlueprint, path normalization, name well-formedness, BlueprintRef
// JSON, pin-type reverse mapping).
//
// Arg parsing + output mirror the material / asset families: each handler
// parses the raw POST body into an FJsonObject and emits a pre-serialized JSON
// string handed to FUnrealOpenMcpToolDispatchResult::Output. The registry/
// dispatch spine stays raw-body — only the handler layer parses.
//
// Behavior reference (read-only): Unreal-MCP's blueprint-create / blueprint-get
// / blueprint-add-component / blueprint-remove-component /
// blueprint-add-variable / blueprint-modify-variable / blueprint-set-default /
// blueprint-add-function / blueprint-add-event (UnrealMcpBlueprintTools.cpp).
// The any-UObject collision probe (so a non-Blueprint asset at the target path
// does not trip the engine's fatal "same fully-qualified name, different class"
// allocation check), the MarkAsGarbage cleanup on a failed create, the
// AssetRegistry.AssetCreated + MarkPackageDirty registration, the disabled-
// ghost-event `enabled` flag, the CreateNewGraph outer-name hijack probe, the
// FunctionCanBePlacedAsEvent gate, the disabled-ghost enable resolution, and
// AddDefaultEventNode were studied for correct Kismet editor API usage and
// adapted to this port's Ok/Fail result shape.
//
// Every handler registered here runs ON THE GAME THREAD (the HTTP server
// marshals dispatch through the GameThreadDispatcher).
#include "Tools/UnrealOpenMcpBlueprintTools.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Tools/UnrealOpenMcpObjectRef.h"   // FUnrealOpenMcpObjectRef::ResolveClass — parent-class resolution
#include "UnrealOpenMcpLog.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
// blueprint_spawn — spawn the GeneratedClass into the editor level. Headless-
// safe (UWorld::SpawnActor, not the viewport-aware editor subsystem). Needs
// AActor + SetActorLabelUnique + FScopedTransaction + UWorld.
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "ActorEditorUtils.h"
#include "ScopedTransaction.h"

#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Kismet2/CompilerResultsLog.h"   // FCompilerResultsLog — blueprint_compile structured diagnostics
#include "Logging/TokenizedMessage.h"     // FTokenizedMessage / IMessageToken / EMessageToken / EMessageSeverity
#include "Misc/UObjectToken.h"            // FUObjectToken — node/graph attribution from compiler UObject tokens
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node_Event.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/Object.h"            // FPropertyChangedEvent + PreEditChange/PostEditChangeProperty
#include "Misc/PackageName.h"
#include "Misc/OutputDevice.h"         // FOutputDevice base for the import-error capture device

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	/** Parse the raw POST body into a JSON object. Empty body → empty object;
	 *  malformed body → null (caller surfaces `invalid_parameter`). Same
	 *  contract as the material / asset families' ParseBody. */
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
	 *  the result is always valid JSON). Same shape as the material family. */
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

	/**
	 * Reject the engine/script/temp content roots for WRITE operations. Mirrors
	 * the asset / material families' IsWritableContentRoot (kept local; the
	 * Blueprint family owns its own copy rather than widening a public header).
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
	 * Root guard for an already-resolved Blueprint. Every Blueprint MUTATOR must
	 * call this after ResolveBlueprint: only blueprint_create used to check a
	 * root, so add-component / remove-component / add-variable / modify-variable
	 * / set-default would happily mutate (and dirty) a Blueprint under /Engine,
	 * /Script or /Temp. The material family guards its mutation target for the
	 * same reason — even save:false dirties the package.
	 *
	 * Returns true when the Blueprint's own package is writable. On false,
	 * OutPackageName carries the offending package for the error message.
	 */
	bool IsBlueprintInWritableRoot(const UBlueprint* Blueprint, FString& OutPackageName)
	{
		if (Blueprint == nullptr)
		{
			OutPackageName.Reset();
			return false;
		}
		const UPackage* Package = Blueprint->GetOutermost();
		if (Package == nullptr)
		{
			OutPackageName.Reset();
			return false;
		}
		OutPackageName = Package->GetName();
		return IsWritableContentRoot(OutPackageName);
	}

	/**
	 * Normalize a caller-supplied Blueprint path into a valid package name +
	 * short asset name. Accepts either a package path ("/Game/Mcp/BP_Thing") or
	 * the object-path form ("/Game/Mcp/BP_Thing.BP_Thing"); strips the
	 * object-name suffix and validates the long package name via FPackageName.
	 * Returns false (with an OutError) when the package name is not valid.
	 */
	bool NormalizeBlueprintPackagePath(
		const FString& InPath,
		FString& OutPackageName,
		FString& OutAssetName,
		FString& OutError)
	{
		FString PackageName = InPath;
		int32 DotIndex = INDEX_NONE;
		if (InPath.FindChar(TEXT('.'), DotIndex))
		{
			PackageName = InPath.Left(DotIndex);
		}

		FText PackageError;
		if (!FPackageName::IsValidLongPackageName(PackageName, /*bIncludeReadOnlyRoots*/ false, &PackageError))
		{
			OutError = FString::Printf(TEXT("invalid Blueprint package path '%s': %s"), *PackageName, *PackageError.ToString());
			return false;
		}

		const FString AssetName = FPackageName::GetShortName(PackageName);
		if (AssetName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("invalid Blueprint path '%s'."), *InPath);
			return false;
		}

		OutPackageName = PackageName;
		OutAssetName = AssetName;
		return true;
	}

	/**
	 * Reject names that are empty, too long, or contain invalid characters,
	 * using the same FKismetNameValidator the Blueprint editor applies. Name
	 * COLLISIONS are intentionally NOT rejected here — each caller owns a
	 * collision check with a tool-specific message — so an in-use result passes
	 * through as well-formed. Shared by the later P6 sub-plans (variables,
	 * components, functions, events) via the header.
	 *
	 * Behavior reference (read-only): Unreal-MCP's IsNameWellFormed.
	 */
	bool IsNameWellFormed(UBlueprint* Blueprint, const FString& Name, FString& OutError)
	{
		const EValidatorResult Result = FKismetNameValidator(Blueprint).IsValid(Name);
		if (Result == EValidatorResult::Ok || Result == EValidatorResult::AlreadyInUse
			|| Result == EValidatorResult::ExistingName || Result == EValidatorResult::LocallyInUse)
		{
			return true;
		}
		OutError = FString::Printf(TEXT("invalid name '%s': %s"), *Name,
			*INameValidatorInterface::GetErrorString(Name, Result));
		return false;
	}

	/**
	 * Read a `{x,y,z}` object field as an FVector. blueprint_spawn uses the same
	 * loose-typed convention as actor_create's ReadVectorField: a missing field
	 * returns the default, a missing axis falls back to the default's component,
	 * so a caller that sends only `{x,y}` still gets a sane vector.
	 */
	FVector ReadVectorField(const TSharedPtr<FJsonObject>& Args, const FString& FieldName, const FVector& Default)
	{
		if (!Args.IsValid() || !Args->HasTypedField<EJson::Object>(FieldName))
		{
			return Default;
		}
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		Args->TryGetObjectField(FieldName, ObjPtr);
		if (ObjPtr == nullptr || !ObjPtr->IsValid())
		{
			return Default;
		}
		const TSharedPtr<FJsonObject>& Obj = *ObjPtr;
		FVector Out = Default;
		if (Obj->HasTypedField<EJson::Number>(TEXT("x")))
		{
			Out.X = Obj->GetNumberField(TEXT("x"));
		}
		if (Obj->HasTypedField<EJson::Number>(TEXT("y")))
		{
			Out.Y = Obj->GetNumberField(TEXT("y"));
		}
		if (Obj->HasTypedField<EJson::Number>(TEXT("z")))
		{
			Out.Z = Obj->GetNumberField(TEXT("z"));
		}
		return Out;
	}

	/** Serialize a UBlueprint into the compact { name, path, parentClass } ref
	 *  shape the create result + later tools emit. Behavior reference
	 *  (read-only): Unreal-MCP's BlueprintRefStruct. */
	TSharedRef<FJsonObject> BlueprintRefToJson(UBlueprint* Blueprint)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Blueprint->GetName());
		Obj->SetStringField(TEXT("path"), Blueprint->GetPathName());
		if (Blueprint->ParentClass)
		{
			Obj->SetStringField(TEXT("parentClass"), Blueprint->ParentClass->GetPathName());
		}
		return Obj;
	}

	// ---- §3.2 pin-type forward mapping (type string -> FEdGraphPinType) --------
	//
	// The forward twin of the FUnrealOpenMcpBlueprintHelpers::PinTypeToString
	// reverse map that blueprint_get already uses. Shared by the variable add +
	// modify handlers so the type tokens round-trip through the same string space
	// the read surface reports. Behavior reference (read-only): Unreal-MCP's
	// MakePinType.

	/** Map a friendly type token to a Blueprint pin type. Recognised primitives
	 *  plus the common math structs; anything else is resolved as an object /
	 *  struct path (a UClass via the shared resolver, or a UScriptStruct via a
	 *  quiet load). Returns false when the token names an object/struct path that
	 *  cannot be resolved. `bIsArray` wraps the type in an array container. */
	bool MakePinType(const FString& InType, bool bIsArray, FEdGraphPinType& OutPinType, FString& OutError)
	{
		const FString Type = InType.TrimStartAndEnd();
		const FString Lower = Type.ToLower();

		auto Set = [&OutPinType](FName Category, FName SubCategory, UObject* SubObject)
		{
			OutPinType = FEdGraphPinType();
			OutPinType.PinCategory = Category;
			OutPinType.PinSubCategory = SubCategory;
			OutPinType.PinSubCategoryObject = SubObject;
		};

		if (Lower == TEXT("bool") || Lower == TEXT("boolean"))
			Set(UEdGraphSchema_K2::PC_Boolean, NAME_None, nullptr);
		else if (Lower == TEXT("int") || Lower == TEXT("integer") || Lower == TEXT("int32"))
			Set(UEdGraphSchema_K2::PC_Int, NAME_None, nullptr);
		else if (Lower == TEXT("int64"))
			Set(UEdGraphSchema_K2::PC_Int64, NAME_None, nullptr);
		else if (Lower == TEXT("byte"))
			Set(UEdGraphSchema_K2::PC_Byte, NAME_None, nullptr);
		// UE5 splits real types across two PC_Real subcategories: PC_Float
		// (single-precision float) and PC_Double (double-precision). Mapping
		// both to PC_Double silently widened every `float` member variable to
		// double. `float`/`single` → PC_Float; `double`/`number`/`real` →
		// PC_Double so the round-trip with PinTypeToString (which mirrors the
		// subcategory) stays symmetric.
		else if (Lower == TEXT("float") || Lower == TEXT("single"))
			Set(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Float, nullptr);
		else if (Lower == TEXT("double") || Lower == TEXT("number") || Lower == TEXT("real"))
			Set(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double, nullptr);
		else if (Lower == TEXT("string"))
			Set(UEdGraphSchema_K2::PC_String, NAME_None, nullptr);
		else if (Lower == TEXT("name"))
			Set(UEdGraphSchema_K2::PC_Name, NAME_None, nullptr);
		else if (Lower == TEXT("text"))
			Set(UEdGraphSchema_K2::PC_Text, NAME_None, nullptr);
		else if (Lower == TEXT("vector"))
			Set(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FVector>::Get());
		else if (Lower == TEXT("vector2d"))
			Set(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FVector2D>::Get());
		else if (Lower == TEXT("rotator"))
			Set(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FRotator>::Get());
		else if (Lower == TEXT("transform"))
			Set(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FTransform>::Get());
		else if (Lower == TEXT("color") || Lower == TEXT("linearcolor"))
			Set(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FLinearColor>::Get());
		else
		{
			// Object / class / struct path: resolve to a UClass (object ref) via
			// the shared resolver, or a UScriptStruct via a quiet load. A miss on
			// both turns into a structured invalid_type rather than an assert.
			if (UClass* Class = FUnrealOpenMcpObjectRef::ResolveClass(Type))
				Set(UEdGraphSchema_K2::PC_Object, NAME_None, Class);
			else if (UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *Type, nullptr, LOAD_NoWarn | LOAD_Quiet))
				Set(UEdGraphSchema_K2::PC_Struct, NAME_None, Struct);
			else
			{
				OutError = FString::Printf(TEXT("unknown variable type '%s' (expected a primitive, a math struct, or a resolvable object/struct path)"), *Type);
				return false;
			}
		}

		OutPinType.ContainerType = bIsArray ? EPinContainerType::Array : EPinContainerType::None;
		return true;
	}
}

// ---- Shared helper implementations exposed via the header ------------------

UBlueprint* FUnrealOpenMcpBlueprintHelpers::ResolveBlueprint(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return nullptr;
	}
	// Prefer an already-in-memory object — assets created earlier this session
	// are registered with the AssetRegistry but not yet saved to disk, so a
	// LoadObject would miss them.
	if (UBlueprint* Found = FindObject<UBlueprint>(nullptr, *Path))
	{
		return Found;
	}
	// LOAD_NoWarn | LOAD_Quiet: a missing asset is an expected outcome here
	// (the caller turns it into a structured "not found" error); without the
	// flags the speculative load warn-spams the editor log.
	return LoadObject<UBlueprint>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

FString FUnrealOpenMcpBlueprintHelpers::PinTypeToString(const FEdGraphPinType& PinType)
{
	const FName Cat = PinType.PinCategory;
	FString Base;
	if (Cat == UEdGraphSchema_K2::PC_Boolean) Base = TEXT("bool");
	else if (Cat == UEdGraphSchema_K2::PC_Int) Base = TEXT("int");
	else if (Cat == UEdGraphSchema_K2::PC_Int64) Base = TEXT("int64");
	else if (Cat == UEdGraphSchema_K2::PC_Byte) Base = TEXT("byte");
	// PC_Real carries the precision in PinSubCategory: PC_Float → single,
	// PC_Double → double. Round-trip with MakePinType's forward map.
	else if (Cat == UEdGraphSchema_K2::PC_Real)
		Base = (PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double) ? TEXT("double") : TEXT("float");
	else if (Cat == UEdGraphSchema_K2::PC_String) Base = TEXT("string");
	else if (Cat == UEdGraphSchema_K2::PC_Name) Base = TEXT("name");
	else if (Cat == UEdGraphSchema_K2::PC_Text) Base = TEXT("text");
	else if (Cat == UEdGraphSchema_K2::PC_Struct)
		Base = PinType.PinSubCategoryObject.IsValid() ? PinType.PinSubCategoryObject->GetName() : TEXT("struct");
	else if (Cat == UEdGraphSchema_K2::PC_Object || Cat == UEdGraphSchema_K2::PC_Class)
		Base = PinType.PinSubCategoryObject.IsValid() ? PinType.PinSubCategoryObject->GetName() : TEXT("object");
	else
		Base = Cat.ToString();

	if (PinType.ContainerType == EPinContainerType::Array)
		return Base + TEXT("[]");
	if (PinType.ContainerType == EPinContainerType::Set)
		return Base + TEXT("{}");
	if (PinType.ContainerType == EPinContainerType::Map)
		return Base + TEXT("<map>");
	return Base;
}

// ---- Tool registration ----------------------------------------------------

void FUnrealOpenMcpBlueprintTools::Register(FUnrealOpenMcpToolRegistry& Registry)
{
	// =========================================================================
	// unreal_open_mcp_blueprint_create — new Blueprint class from a parent.
	// =========================================================================
	//
	// `path` is the /Game package path for the new asset (object-path form also
	// accepted and normalised). `parent_class` defaults to Actor; other
	// Blueprintable parents allowed when CanCreateBlueprintOfClass passes. The
	// asset is registered in-session (AssetRegistry.AssetCreated +
	// MarkPackageDirty); no disk save.
	//
	// The any-UObject collision probe runs BEFORE CreatePackage: a non-Blueprint
	// asset (e.g. an on-disk UStaticMesh) at the target path slips past a
	// UBlueprint-scoped probe (class mismatch -> null), CreatePackage then
	// returns the existing package, and CreateBlueprint -> NewObject<UBlueprint>
	// hits the engine's fatal "same fully qualified name, different class"
	// allocation check instead of a structured error. Probing for ANY UObject
	// turns that into a structured asset_already_exists.
	//
	// Mutating. The gate's mandatory `paths_hint` is enforced by the dispatcher
	// BEFORE the handler runs. Structured errors:
	//   - invalid_parameter          — malformed body
	//   - missing_parameter          — `path` absent
	//   - parent_class_not_found     — parent class did not resolve
	//   - parent_not_blueprintable   — CanCreateBlueprintOfClass returned false
	//   - invalid_package_path       — path is not a valid long package name
	//   - invalid_content_root       — path under /Engine, /Script, /Temp
	//   - asset_already_exists       — any UObject at the target object path
	//   - create_failed              — CreateBlueprint returned null
	Registry.Register(
		TEXT("unreal_open_mcp_blueprint_create"),
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
					TEXT("'path' is required (the new Blueprint package path, e.g. '/Game/Mcp/BP_Thing')."));
			}

			// Accept either `parent_class` (preferred snake_case) or the
			// `parentClass` camelCase alias; default to Actor. The bridge parses
			// args by key name, so accept both and pin tests to snake_case.
			FString ParentPath = TEXT("/Script/Engine.Actor");
			if (Args->HasTypedField<EJson::String>(TEXT("parent_class")))
			{
				ParentPath = Args->GetStringField(TEXT("parent_class"));
			}
			else if (Args->HasTypedField<EJson::String>(TEXT("parentClass")))
			{
				ParentPath = Args->GetStringField(TEXT("parentClass"));
			}

			UClass* ParentClass = FUnrealOpenMcpObjectRef::ResolveClass(ParentPath);
			if (!ParentClass)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("parent_class_not_found"),
					FString::Printf(TEXT("Could not resolve parent class '%s'."), *ParentPath));
			}
			if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("parent_not_blueprintable"),
					FString::Printf(TEXT("'%s' cannot be used as a Blueprint parent class."), *ParentPath));
			}

			FString PackageName;
			FString AssetName;
			FString PathError;
			if (!NormalizeBlueprintPackagePath(Path, PackageName, AssetName, PathError))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_package_path"),
					PathError);
			}

			if (!IsWritableContentRoot(PackageName))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_content_root"),
					FString::Printf(
						TEXT("Refusing to create '%s' under an engine content root; use a project root like '/Game'."),
						*PackageName));
			}

			// Probe for ANY UObject, not just a UBlueprint (see file-header
			// comment for why a class-scoped probe is unsafe here).
			const FString ObjectPath = PackageName + TEXT(".") + AssetName;
			if (FindObject<UObject>(nullptr, *ObjectPath) != nullptr
				|| LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet) != nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("asset_already_exists"),
					FString::Printf(TEXT("An asset already exists at '%s'."), *ObjectPath));
			}

			UPackage* Package = CreatePackage(*PackageName);
			if (!Package)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("create_failed"),
					FString::Printf(TEXT("Could not create package '%s'."), *PackageName));
			}

			UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
				ParentClass, Package, FName(*AssetName), BPTYPE_Normal,
				UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
				FName(TEXT("UnrealOpenMcpBlueprintCreate")));
			if (!Blueprint)
			{
				// CreateBlueprint failed after the package was minted; drop the
				// now-empty package so the path isn't left half-claimed in
				// memory for the rest of the session.
				Package->MarkAsGarbage();
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("create_failed"),
					TEXT("FKismetEditorUtilities::CreateBlueprint returned null."));
			}

			FAssetRegistryModule::AssetCreated(Blueprint);
			Package->MarkPackageDirty();

			UE_LOG(
				LogUnrealOpenMcp, Log,
				TEXT("[Unreal Open MCP] blueprint_create: '%s' (parent %s)."),
				*Blueprint->GetPathName(), *ParentClass->GetName());

			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(BlueprintRefToJson(Blueprint))));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// =========================================================================
	// unreal_open_mcp_blueprint_get — read-only scoped graph summary.
	// =========================================================================
	//
	// `path` is the Blueprint asset object path (package-path form also accepted
	// via ResolveBlueprint's FindObject-first chain). Returns a summary DTO:
	//   - name / path / parentClass
	//   - variables[]    — { name, type, isArray, default? } from NewVariables
	//   - components[]   — { name, class, parent } from the SCS (parent is the
	//                      attach parent; empty for a root node — the LLM needs
	//                      it to drive P6.4's add-component parentComponent)
	//   - functions[]    — { name, nodeCount } from FunctionGraphs
	//   - events[]       — { name, enabled } from UK2Node_Event across the
	//                      ubergraph pages. Fresh Actor Blueprints are pre-seeded
	//                      with DISABLED ghost event nodes (ReceiveBeginPlay /
	//                      ReceiveTick / ...); `enabled` mirrors the
	//                      add-event semantics so an agent does not conclude a
	//                      disabled ghost already fires.
	//   - interfaces[]   — implemented interface class paths
	//   - parentChain[]  — parent class names up to UObject
	//
	// Read-only — no gate, no paths_hint surface. Structured errors:
	//   - invalid_parameter   — malformed body
	//   - missing_parameter   — `path` absent
	//   - blueprint_not_found — no Blueprint at `path`
	Registry.Register(
		TEXT("unreal_open_mcp_blueprint_get"),
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
					TEXT("'path' is required (Blueprint asset object path)."));
			}

			UBlueprint* Blueprint = FUnrealOpenMcpBlueprintHelpers::ResolveBlueprint(Path);
			if (!Blueprint)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("blueprint_not_found"),
					FString::Printf(TEXT("Blueprint not found at '%s'."), *Path));
			}

			TSharedRef<FJsonObject> Out = BlueprintRefToJson(Blueprint);

			// Variables — NewVariables holds member vars added via the builder/
			// UI; visible pre-compile.
			TArray<TSharedPtr<FJsonValue>> Variables;
			for (const FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				TSharedRef<FJsonObject> V = MakeShared<FJsonObject>();
				V->SetStringField(TEXT("name"), Var.VarName.ToString());
				V->SetStringField(TEXT("type"), FUnrealOpenMcpBlueprintHelpers::PinTypeToString(Var.VarType));
				V->SetBoolField(TEXT("isArray"), Var.VarType.ContainerType == EPinContainerType::Array);
				if (!Var.DefaultValue.IsEmpty())
				{
					V->SetStringField(TEXT("default"), Var.DefaultValue);
				}
				Variables.Add(MakeShared<FJsonValueObject>(V));
			}
			Out->SetArrayField(TEXT("variables"), Variables);

			// SCS components.
			TArray<TSharedPtr<FJsonValue>> Components;
			if (Blueprint->SimpleConstructionScript)
			{
				for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
				{
					if (!Node)
					{
						continue;
					}
					TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
					C->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
					C->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT(""));
					// 'parent' is the SCS attach parent (empty for a root node).
					if (USCS_Node* ParentNode = Blueprint->SimpleConstructionScript->FindParentNode(Node))
					{
						C->SetStringField(TEXT("parent"), ParentNode->GetVariableName().ToString());
					}
					else
					{
						C->SetStringField(TEXT("parent"), TEXT(""));
					}
					Components.Add(MakeShared<FJsonValueObject>(C));
				}
			}
			Out->SetArrayField(TEXT("components"), Components);

			// Functions (user function graphs) with node-counts.
			TArray<TSharedPtr<FJsonValue>> Functions;
			for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				if (!Graph)
				{
					continue;
				}
				TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
				F->SetStringField(TEXT("name"), Graph->GetName());
				F->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());
				Functions.Add(MakeShared<FJsonValueObject>(F));
			}
			Out->SetArrayField(TEXT("functions"), Functions);

			// Events (UK2Node_Event nodes across the ubergraph pages).
			TArray<TSharedPtr<FJsonValue>> Events;
			for (const UEdGraph* Graph : Blueprint->UbergraphPages)
			{
				if (!Graph)
				{
					continue;
				}
				for (const UEdGraphNode* GraphNode : Graph->Nodes)
				{
					if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(GraphNode))
					{
						TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
						E->SetStringField(TEXT("name"), EventNode->EventReference.GetMemberName().ToString());
						E->SetBoolField(TEXT("enabled"), EventNode->IsNodeEnabled());
						Events.Add(MakeShared<FJsonValueObject>(E));
					}
				}
			}
			Out->SetArrayField(TEXT("events"), Events);

			// Implemented interfaces.
			TArray<TSharedPtr<FJsonValue>> Interfaces;
			for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
			{
				if (Iface.Interface)
				{
					Interfaces.Add(MakeShared<FJsonValueString>(Iface.Interface->GetPathName()));
				}
			}
			Out->SetArrayField(TEXT("interfaces"), Interfaces);

			// Parent chain (parent class up to UObject).
			TArray<TSharedPtr<FJsonValue>> ParentChain;
			for (UClass* C = Blueprint->ParentClass; C; C = C->GetSuperClass())
			{
				ParentChain.Add(MakeShared<FJsonValueString>(C->GetName()));
			}
			Out->SetArrayField(TEXT("parentChain"), ParentChain);

			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Out)));
		});

	// =========================================================================
	// unreal_open_mcp_blueprint_add_component — add a node to the SCS graph.
	// =========================================================================
	//
	// `path` is the Blueprint asset object path (package-path form also accepted
	// via ResolveBlueprint's FindObject-first chain). `component_class` is a
	// UActorComponent subclass path or short name resolved via the shared
	// FUnrealOpenMcpObjectRef::ResolveClass. `name` is the new component's
	// variable name. Optional `parent_component` attaches the new node under an
	// existing SCS scene-component node; omitted → added as a root node.
	//
	// Guards (each maps to a structured error code, never an engine assert):
	//   - no SimpleConstructionScript   → no_scs
	//   - class not a UActorComponent   → invalid_component_class
	//   - abstract / deprecated class   → abstract_component
	//     (SCS CreateNode -> NewObject on such a class fatally asserts, e.g.
	//     '/Script/Engine.LightComponentBase'; the ClassFlags check runs BEFORE
	//     CreateNode so the fatal path is unreachable)
	//   - name collision in the SCS     → name_collision
	//   - name collides with a member var → name_collision
	//     (SCS component names, member-variable names, and inherited parent
	//     properties all share the generated class's property namespace, so a
	//     clash only surfaces later at blueprint-compile — pre-check so the
	//     agent gets a structured error instead of a delayed compile failure)
	//   - name collides with a parent
	//     class property                 → name_collision
	//   - parent_component not found     → parent_not_found
	//   - new class or parent not a
	//     USceneComponent                → invalid_attachment
	//     (attachment is a scene-graph op: both sides must be USceneComponents,
	//     otherwise AddChildNode produces a non-attachable hierarchy)
	//
	// On success: USCS_Node minted via CreateNode, attached under the parent
	// (AddChildNode) or added as a root (AddNode), then
	// MarkBlueprintAsStructurallyModified so a later compile rebuilds the CDO.
	//
	// Mutating. The gate's mandatory `paths_hint` is enforced by the dispatcher
	// BEFORE the handler runs.
	Registry.Register(
		TEXT("unreal_open_mcp_blueprint_add_component"),
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
					TEXT("'path' is required (Blueprint asset object path)."));
			}

			UBlueprint* Blueprint = FUnrealOpenMcpBlueprintHelpers::ResolveBlueprint(Path);
			if (!Blueprint)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("blueprint_not_found"),
					FString::Printf(TEXT("Blueprint not found at '%s'."), *Path));
			}

			// Mutators must refuse the engine/script/temp content roots: the
			// mutation below dirties the Blueprint's package even when the caller
			// passes save:false. Only blueprint_create used to check a root, so
			// every other Blueprint mutator could edit /Engine content.
			FString BlueprintPackageName;
			if (!IsBlueprintInWritableRoot(Blueprint, BlueprintPackageName))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_content_root"),
					FString::Printf(
						TEXT("Refusing to modify '%s' under a reserved content root; ")
						TEXT("use a project root like '/Game'."),
						*BlueprintPackageName));
			}

			USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
			if (!SCS)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_scs"),
					FString::Printf(TEXT("'%s' has no Simple Construction Script (not an Actor-based Blueprint?)."), *Blueprint->GetName()));
			}

			const FString CompName = Args->HasTypedField<EJson::String>(TEXT("name"))
				? Args->GetStringField(TEXT("name"))
				: FString();
			if (CompName.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'name' is required (variable name for the new component)."));
			}

			FString NameError;
			if (!IsNameWellFormed(Blueprint, CompName, NameError))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("invalid_name"), NameError);
			}

			// Resolve the component class. Accept both `component_class`
			// (snake_case) and the `componentClass` camelCase alias the way the
			// create tool accepts parent_class / parentClass.
			FString CompClassPath;
			if (Args->HasTypedField<EJson::String>(TEXT("component_class")))
			{
				CompClassPath = Args->GetStringField(TEXT("component_class"));
			}
			else if (Args->HasTypedField<EJson::String>(TEXT("componentClass")))
			{
				CompClassPath = Args->GetStringField(TEXT("componentClass"));
			}
			if (CompClassPath.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'component_class' is required (UActorComponent subclass path or short name)."));
			}

			UClass* CompClass = FUnrealOpenMcpObjectRef::ResolveClass(CompClassPath);
			if (!CompClass || !CompClass->IsChildOf(UActorComponent::StaticClass()))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_component_class"),
					FString::Printf(TEXT("'%s' is not a UActorComponent subclass."), *CompClassPath));
			}
			// SCS CreateNode -> NewObject on the class; an abstract/deprecated
			// class fatally asserts there (e.g. LightComponentBase). Reject up
			// front with a structured error so the fatal path is unreachable.
			if (CompClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("abstract_component"),
					FString::Printf(TEXT("'%s' is abstract/deprecated and cannot be instantiated as a component."), *CompClass->GetName()));
			}

			// Cross-namespace collision checks. SCS component names, member-
			// variable names, and inherited parent properties all share the
			// generated class's property namespace, so a clash only fails later
			// at blueprint-compile. Pre-check each namespace and surface a
			// structured name_collision instead of a delayed compile failure.
			if (SCS->FindSCSNode(FName(*CompName)) != nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("name_collision"),
					FString::Printf(TEXT("a component named '%s' already exists."), *CompName));
			}
			if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*CompName)) != INDEX_NONE)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("name_collision"),
					FString::Printf(TEXT("a variable named '%s' already exists, so a component cannot reuse that name."), *CompName));
			}
			if (Blueprint->ParentClass && FindFProperty<FProperty>(Blueprint->ParentClass, FName(*CompName)) != nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("name_collision"),
					FString::Printf(TEXT("'%s' is already a property on parent class '%s', so a component cannot reuse that name."), *CompName, *Blueprint->ParentClass->GetName()));
			}

			// Resolve the optional attach parent. Empty → root node.
			FString ParentName;
			if (Args->HasTypedField<EJson::String>(TEXT("parent_component")))
			{
				ParentName = Args->GetStringField(TEXT("parent_component"));
			}
			else if (Args->HasTypedField<EJson::String>(TEXT("parentComponent")))
			{
				ParentName = Args->GetStringField(TEXT("parentComponent"));
			}
			USCS_Node* ParentNode = ParentName.IsEmpty() ? nullptr : SCS->FindSCSNode(FName(*ParentName));
			if (!ParentName.IsEmpty() && !ParentNode)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("parent_not_found"),
					FString::Printf(TEXT("parent component '%s' not found."), *ParentName));
			}
			// The SCS root is a scene-graph node and MUST be a USceneComponent;
			// AddNode on a non-scene root either asserts at compile or is silently
			// swapped for the DefaultSceneRoot. Reject a non-scene component up
			// front even when no parent is supplied (root path).
			if (!CompClass->IsChildOf(USceneComponent::StaticClass()))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_attachment"),
					FString::Printf(TEXT("'%s' is not a USceneComponent, so it cannot live on the Simple Construction Script (only scene components attach in the component hierarchy)."), *CompClass->GetName()));
			}
			// Attachment is a scene-graph operation: both the new component and
			// its parent must be USceneComponents, otherwise AddChildNode
			// produces an invalid (non-attachable) hierarchy.
			if (ParentNode)
			{
				if (!ParentNode->ComponentClass || !ParentNode->ComponentClass->IsChildOf(USceneComponent::StaticClass()))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_attachment"),
						FString::Printf(TEXT("parent component '%s' is not a USceneComponent and cannot host child components."), *ParentName));
				}
			}

			USCS_Node* NewNode = SCS->CreateNode(CompClass, FName(*CompName));
			if (!NewNode)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("create_failed"),
					TEXT("USimpleConstructionScript::CreateNode returned null."));
			}

			if (ParentNode)
			{
				ParentNode->AddChildNode(NewNode);
			}
			else
			{
				SCS->AddNode(NewNode);
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

			UE_LOG(
				LogUnrealOpenMcp, Log,
				TEXT("[Unreal Open MCP] blueprint_add_component: '%s' (%s)%s on '%s'."),
				*CompName, *CompClass->GetName(),
				ParentNode ? *FString::Printf(TEXT(" under '%s'"), *ParentName) : TEXT(""),
				*Blueprint->GetName());

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("component"), CompName);
			Out->SetStringField(TEXT("class"), CompClass->GetName());
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Out)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// =========================================================================
	// unreal_open_mcp_blueprint_remove_component — delete an SCS node.
	// =========================================================================
	//
	// `path` is the Blueprint asset object path; `name` is the variable name of
	// the SCS node to remove. RemoveNodeAndPromoteChildren deletes the node and
	// re-parents any child scene-component nodes onto the removed node's parent
	// (so a subtree is never orphaned). MarkBlueprintAsStructurallyModified
	// follows so a later compile rebuilds the CDO without the component.
	//
	// Structured errors:
	//   - blueprint_not_found  — no Blueprint at `path`
	//   - no_scs               — Blueprint has no SimpleConstructionScript
	//   - component_not_found  — no SCS node with that variable name
	//
	// Mutating. The gate's mandatory `paths_hint` is enforced by the dispatcher
	// BEFORE the handler runs.
	Registry.Register(
		TEXT("unreal_open_mcp_blueprint_remove_component"),
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
					TEXT("'path' is required (Blueprint asset object path)."));
			}

			UBlueprint* Blueprint = FUnrealOpenMcpBlueprintHelpers::ResolveBlueprint(Path);
			if (!Blueprint)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("blueprint_not_found"),
					FString::Printf(TEXT("Blueprint not found at '%s'."), *Path));
			}

			// Mutators must refuse the engine/script/temp content roots: the
			// mutation below dirties the Blueprint's package even when the caller
			// passes save:false. Only blueprint_create used to check a root, so
			// every other Blueprint mutator could edit /Engine content.
			FString BlueprintPackageName;
			if (!IsBlueprintInWritableRoot(Blueprint, BlueprintPackageName))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_content_root"),
					FString::Printf(
						TEXT("Refusing to modify '%s' under a reserved content root; ")
						TEXT("use a project root like '/Game'."),
						*BlueprintPackageName));
			}

			USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
			if (!SCS)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_scs"),
					FString::Printf(TEXT("'%s' has no Simple Construction Script."), *Blueprint->GetName()));
			}

			const FString CompName = Args->HasTypedField<EJson::String>(TEXT("name"))
				? Args->GetStringField(TEXT("name"))
				: FString();
			if (CompName.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'name' is required (variable name of the SCS component node to remove)."));
			}

			USCS_Node* Node = SCS->FindSCSNode(FName(*CompName));
			if (!Node)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("component_not_found"),
					FString::Printf(TEXT("component '%s' not found on '%s'."), *CompName, *Blueprint->GetName()));
			}

			SCS->RemoveNodeAndPromoteChildren(Node);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

			UE_LOG(
				LogUnrealOpenMcp, Log,
				TEXT("[Unreal Open MCP] blueprint_remove_component: '%s' from '%s'."),
				*CompName, *Blueprint->GetName());

			return FUnrealOpenMcpToolDispatchResult::Ok();
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// =========================================================================
	// unreal_open_mcp_blueprint_add_variable — typed member variable.
	// =========================================================================
	//
	// `path` is the Blueprint asset object path (package-path form also accepted
	// via ResolveBlueprint's FindObject-first chain). `name` is the new member
	// variable name; `type` is a pin-type token (bool/int/int64/byte/float/double/
	// string/name/text/vector/vector2d/rotator/transform/color, or a resolvable
	// object/struct path). Optional `is_array` wraps the type in an array.
	// Optional `default_value` is stored on the variable descriptor (UE text
	// format — it only takes effect after a compile lands the property on the
	// generated class). Uses FBlueprintEditorUtils::AddMemberVariable.
	//
	// Guards (each maps to a structured error code):
	//   - name collision with an existing member variable → name_collision
	//   - name collision with an SCS component / parent-class property
	//     → name_collision
	//     (member-variable names, SCS component names, and inherited parent
	//     properties all share the generated class's property namespace, so a
	//     clash only surfaces at blueprint-compile — pre-check so the agent gets
	//     a structured error instead of a delayed compile failure)
	//   - unresolvable type token → invalid_type
	//   - AddMemberVariable refusal → add_failed
	//
	// Mutating. The gate's mandatory `paths_hint` is enforced by the dispatcher
	// BEFORE the handler runs.
	Registry.Register(
		TEXT("unreal_open_mcp_blueprint_add_variable"),
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
					TEXT("'path' is required (Blueprint asset object path)."));
			}

			UBlueprint* Blueprint = FUnrealOpenMcpBlueprintHelpers::ResolveBlueprint(Path);
			if (!Blueprint)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("blueprint_not_found"),
					FString::Printf(TEXT("Blueprint not found at '%s'."), *Path));
			}

			// Mutators must refuse the engine/script/temp content roots: the
			// mutation below dirties the Blueprint's package even when the caller
			// passes save:false. Only blueprint_create used to check a root, so
			// every other Blueprint mutator could edit /Engine content.
			FString BlueprintPackageName;
			if (!IsBlueprintInWritableRoot(Blueprint, BlueprintPackageName))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_content_root"),
					FString::Printf(
						TEXT("Refusing to modify '%s' under a reserved content root; ")
						TEXT("use a project root like '/Game'."),
						*BlueprintPackageName));
			}

			const FString VarName = Args->HasTypedField<EJson::String>(TEXT("name"))
				? Args->GetStringField(TEXT("name"))
				: FString();
			if (VarName.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'name' is required (new variable name)."));
			}
			FString NameError;
			if (!IsNameWellFormed(Blueprint, VarName, NameError))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("invalid_name"), NameError);
			}

			const FString TypeToken = Args->HasTypedField<EJson::String>(TEXT("type"))
				? Args->GetStringField(TEXT("type"))
				: FString();
			if (TypeToken.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'type' is required (a primitive, a math struct, or a resolvable object/struct path)."));
			}

			// Cross-namespace collision checks. Member-variable names, SCS
			// component names, and inherited parent properties all share the
			// generated class's property namespace, so a clash only fails later
			// at blueprint-compile. Pre-check each namespace and surface a
			// structured name_collision instead of a delayed compile failure.
			if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*VarName)) != INDEX_NONE)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("name_collision"),
					FString::Printf(TEXT("a variable named '%s' already exists."), *VarName));
			}
			if (Blueprint->SimpleConstructionScript
				&& Blueprint->SimpleConstructionScript->FindSCSNode(FName(*VarName)) != nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("name_collision"),
					FString::Printf(TEXT("a component named '%s' already exists, so a variable cannot reuse that name."), *VarName));
			}
			if (Blueprint->ParentClass && FindFProperty<FProperty>(Blueprint->ParentClass, FName(*VarName)) != nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("name_collision"),
					FString::Printf(TEXT("'%s' is already a property on parent class '%s', so a variable cannot reuse that name."), *VarName, *Blueprint->ParentClass->GetName()));
			}

			const bool bIsArray = Args->HasTypedField<EJson::Boolean>(TEXT("is_array"))
				? Args->GetBoolField(TEXT("is_array"))
				: false;

			FEdGraphPinType PinType;
			FString PinError;
			if (!MakePinType(TypeToken, bIsArray, PinType, PinError))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("invalid_type"), PinError);
			}

			const FString DefaultValue = Args->HasTypedField<EJson::String>(TEXT("default_value"))
				? Args->GetStringField(TEXT("default_value"))
				: FString();

			if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VarName), PinType, DefaultValue))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("add_failed"),
					FString::Printf(TEXT("AddMemberVariable failed for '%s' (the type may be unsupported as a member variable)."), *VarName));
			}

			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

			UE_LOG(
				LogUnrealOpenMcp, Log,
				TEXT("[Unreal Open MCP] blueprint_add_variable: '%s' (%s) on '%s'."),
				*VarName, *FUnrealOpenMcpBlueprintHelpers::PinTypeToString(PinType), *Blueprint->GetName());

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("variable"), VarName);
			Out->SetStringField(TEXT("type"), FUnrealOpenMcpBlueprintHelpers::PinTypeToString(PinType));
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Out)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// =========================================================================
	// unreal_open_mcp_blueprint_modify_variable — rename and/or retype.
	// =========================================================================
	//
	// `path` is the Blueprint asset object path; `name` is the existing member
	// variable. At least one of `new_name` / `new_type` is required. `new_type`
	// re-parses a pin-type token (with an optional `is_array`); `new_name`
	// renames. Validate-before-mutate ordering: the rename is validated (well-
	// formed + no collision) BEFORE the retype commits, so a colliding or
	// ill-formed new_name never leaves a partial mutation (retype landed, rename
	// failed). ChangeMemberVariableType commits immediately; RenameMemberVariable
	// is void and does no collision check of its own, so renaming onto an
	// existing name would silently produce duplicate member names that break a
	// later compile — both are pre-checked here.
	//
	// Structured errors:
	//   - variable_not_found   — name names no member variable
	//   - missing_parameter    — neither new_name nor new_type
	//   - name_collision       — new_name already used by a variable/component/
	//                            parent property
	//   - invalid_type         — new_type did not resolve
	//   - invalid_name         — new_name failed the Kismet name validator
	//
	// Mutating. The gate's mandatory `paths_hint` is enforced by the dispatcher
	// BEFORE the handler runs.
	Registry.Register(
		TEXT("unreal_open_mcp_blueprint_modify_variable"),
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
					TEXT("'path' is required (Blueprint asset object path)."));
			}

			UBlueprint* Blueprint = FUnrealOpenMcpBlueprintHelpers::ResolveBlueprint(Path);
			if (!Blueprint)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("blueprint_not_found"),
					FString::Printf(TEXT("Blueprint not found at '%s'."), *Path));
			}

			// Mutators must refuse the engine/script/temp content roots: the
			// mutation below dirties the Blueprint's package even when the caller
			// passes save:false. Only blueprint_create used to check a root, so
			// every other Blueprint mutator could edit /Engine content.
			FString BlueprintPackageName;
			if (!IsBlueprintInWritableRoot(Blueprint, BlueprintPackageName))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_content_root"),
					FString::Printf(
						TEXT("Refusing to modify '%s' under a reserved content root; ")
						TEXT("use a project root like '/Game'."),
						*BlueprintPackageName));
			}

			const FString VarName = Args->HasTypedField<EJson::String>(TEXT("name"))
				? Args->GetStringField(TEXT("name"))
				: FString();
			if (VarName.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'name' is required (existing variable name)."));
			}
			if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*VarName)) == INDEX_NONE)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("variable_not_found"),
					FString::Printf(TEXT("variable '%s' not found on '%s'."), *VarName, *Blueprint->GetName()));
			}

			const FString NewName = Args->HasTypedField<EJson::String>(TEXT("new_name"))
				? Args->GetStringField(TEXT("new_name"))
				: FString();
			const FString NewType = Args->HasTypedField<EJson::String>(TEXT("new_type"))
				? Args->GetStringField(TEXT("new_type"))
				: FString();
			if (NewName.IsEmpty() && NewType.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("Provide at least one of 'new_name' or 'new_type'."));
			}

			FName EffectiveName = FName(*VarName);

			// Validate the rename BEFORE any mutation. ChangeMemberVariableType
			// commits immediately, so if a colliding/ill-formed new_name were
			// checked afterwards the tool would report failure while the type
			// change had already landed — the agent's model and the asset would
			// disagree. RenameMemberVariable is void and does no
			// validity/collision check of its own — renaming onto an existing
			// name silently produces duplicate member names that break a later
			// compile.
			if (!NewName.IsEmpty())
			{
				FString NameError;
				if (!IsNameWellFormed(Blueprint, NewName, NameError))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("invalid_name"), NameError);
				}
				if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*NewName)) != INDEX_NONE)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("name_collision"),
						FString::Printf(TEXT("cannot rename to '%s': a variable with that name already exists."), *NewName));
				}
				if (Blueprint->SimpleConstructionScript
					&& Blueprint->SimpleConstructionScript->FindSCSNode(FName(*NewName)) != nullptr)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("name_collision"),
						FString::Printf(TEXT("cannot rename to '%s': a component with that name already exists."), *NewName));
				}
				if (Blueprint->ParentClass && FindFProperty<FProperty>(Blueprint->ParentClass, FName(*NewName)) != nullptr)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("name_collision"),
						FString::Printf(TEXT("cannot rename to '%s': it is already a property on parent class '%s'."), *NewName, *Blueprint->ParentClass->GetName()));
				}
			}

			if (!NewType.IsEmpty())
			{
				const bool bIsArray = Args->HasTypedField<EJson::Boolean>(TEXT("is_array"))
					? Args->GetBoolField(TEXT("is_array"))
					: false;
				FEdGraphPinType PinType;
				FString PinError;
				if (!MakePinType(NewType, bIsArray, PinType, PinError))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("invalid_type"), PinError);
				}
				FBlueprintEditorUtils::ChangeMemberVariableType(Blueprint, EffectiveName, PinType);
			}
			if (!NewName.IsEmpty())
			{
				FBlueprintEditorUtils::RenameMemberVariable(Blueprint, EffectiveName, FName(*NewName));
				EffectiveName = FName(*NewName);
			}

			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

			UE_LOG(
				LogUnrealOpenMcp, Log,
				TEXT("[Unreal Open MCP] blueprint_modify_variable: '%s' on '%s' (now '%s')."),
				*VarName, *Blueprint->GetName(), *EffectiveName.ToString());

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("variable"), EffectiveName.ToString());
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Out)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// =========================================================================
	// unreal_open_mcp_blueprint_set_default — CDO property write.
	// =========================================================================
	//
	// `path` is the Blueprint asset object path; `property` is a property name on
	// the generated class; `value` is the value in UE text format (numbers,
	// '(X=1,Y=2,Z=3)' for structs, asset paths for object refs). Writes the
	// Class Default Object via the property's own text importer
	// (ImportText_Direct), bracketed with Pre/PostEditChangeProperty so an open
	// Details panel / property-change observers refresh. This changes the CLASS
	// DEFAULT, so it affects newly-spawned instances only — not actors already
	// placed in a level.
	//
	// Compile-first: a member variable added via blueprint_add_variable is NOT
	// yet a property on the generated class until a compile lands it. set_default
	// on such a property reports property_not_found with a message that points at
	// blueprint_compile.
	//
		// Structured errors:
	//   - no_generated_class — Blueprint has no GeneratedClass (compile first)
	//   - no_cdo             — GeneratedClass resolved but its CDO did not
	//                          (rare; class not fully loaded). Distinct from
	//                          no_generated_class so an agent can tell a
	//                          missing-compile case from a CDO-resolution
	//                          failure.
	//   - property_not_found — property absent on the generated class
	//   - import_failed      — ImportText_Direct could not parse the value
	//
	// Mutating. The gate's mandatory `paths_hint` is enforced by the dispatcher
	// BEFORE the handler runs.
	Registry.Register(
		TEXT("unreal_open_mcp_blueprint_set_default"),
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
					TEXT("'path' is required (Blueprint asset object path)."));
			}

			UBlueprint* Blueprint = FUnrealOpenMcpBlueprintHelpers::ResolveBlueprint(Path);
			if (!Blueprint)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("blueprint_not_found"),
					FString::Printf(TEXT("Blueprint not found at '%s'."), *Path));
			}

			// Mutators must refuse the engine/script/temp content roots: the
			// mutation below dirties the Blueprint's package even when the caller
			// passes save:false. Only blueprint_create used to check a root, so
			// every other Blueprint mutator could edit /Engine content.
			FString BlueprintPackageName;
			if (!IsBlueprintInWritableRoot(Blueprint, BlueprintPackageName))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_content_root"),
					FString::Printf(
						TEXT("Refusing to modify '%s' under a reserved content root; ")
						TEXT("use a project root like '/Game'."),
						*BlueprintPackageName));
			}

			UClass* GeneratedClass = Blueprint->GeneratedClass;
			if (!GeneratedClass)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_generated_class"),
					TEXT("Blueprint has no GeneratedClass; compile it first (blueprint_compile)."));
			}

			UObject* CDO = GeneratedClass->GetDefaultObject();
			if (!CDO)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_cdo"),
					TEXT("Could not resolve the Class Default Object for the generated class."));
			}

			const FString PropName = Args->HasTypedField<EJson::String>(TEXT("property"))
				? Args->GetStringField(TEXT("property"))
				: FString();
			if (PropName.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'property' is required (CDO property name)."));
			}
			FProperty* Prop = FindFProperty<FProperty>(GeneratedClass, FName(*PropName));
			if (!Prop)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("property_not_found"),
					FString::Printf(TEXT("property '%s' not found on '%s' (if you just added it via blueprint_add_variable, run blueprint_compile first so it lands on the generated class)."), *PropName, *GeneratedClass->GetName()));
			}

			const FString Value = Args->HasTypedField<EJson::String>(TEXT("value"))
				? Args->GetStringField(TEXT("value"))
				: FString();

			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
			// Bracket the CDO write with Pre/PostEditChangeProperty so any open
			// Details panel or property-change observers refresh — ImportText
			// alone mutates the memory silently. PostEditChangeProperty runs on
			// every path because a parse failure can still leave the value
			// partially written. The error-capture device is a self-contained
			// FOutputDevice (the engine's FStringOutputDevice header home has
			// moved across UE versions), mirroring the ConsoleTools capture
			// pattern.
			class FImportErrorCapture : public FOutputDevice
			{
			public:
				virtual void Serialize(const TCHAR* V, ELogVerbosity::Type, const FName&) override
				{
					if (!Errors.IsEmpty()) Errors += TEXT('\n');
					Errors += V;
				}
				FString Errors;
			};
			FImportErrorCapture ErrorCapture;
			CDO->PreEditChange(Prop);
			const TCHAR* Result = Prop->ImportText_Direct(*Value, ValuePtr, CDO, PPF_None, &ErrorCapture);
			FPropertyChangedEvent ChangeEvent(Prop);
			CDO->PostEditChangeProperty(ChangeEvent);
			if (Result == nullptr || !ErrorCapture.Errors.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("import_failed"),
					FString::Printf(TEXT("Failed to parse value '%s' for property '%s': %s"), *Value, *PropName, *ErrorCapture.Errors));
			}

			CDO->MarkPackageDirty();
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

			UE_LOG(
				LogUnrealOpenMcp, Log,
				TEXT("[Unreal Open MCP] blueprint_set_default: %s.%s = %s (affects new instances only)."),
				*Blueprint->GetName(), *PropName, *Value);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("property"), PropName);
			Out->SetStringField(TEXT("value"), Value);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Out)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// =========================================================================
	// unreal_open_mcp_blueprint_add_function — user function-graph stub.
	// =========================================================================
	//
	// `path` is the Blueprint asset object path (package-path form also accepted
	// via ResolveBlueprint's FindObject-first chain). `name` is the new function
	// graph name. Creates an empty user function graph via
	// FBlueprintEditorUtils::CreateNewGraph + AddFunctionGraph — the K2 schema
	// auto-wires the entry/result nodes, so the agent gets a callable stub ready
	// for a compile. Body authoring (add_node / connect_pins / free-form wiring)
	// is OUT OF SCOPE — this is a stub-only surface.
	//
	// Guards (each maps to a structured error code):
	//   - name not well-formed (Kismet validator) → invalid_name
	//   - name collides with an existing function graph → name_collision
	//   - name collides with any UObject outered to the Blueprint
	//     → name_collision
	//     (CreateNewGraph resolves an outer-name clash by RENAMING the existing
	//     object aside — a name colliding with the EventGraph or any other graph
	//     outered to the Blueprint would silently hijack it and report success;
	//     the pre-check turns that into a structured error)
	//   - CreateNewGraph returns null → create_graph_failed
	//
	// MVP: no custom signature objects. AddFunctionGraph is called with a null
	// SignatureFromObject, so the stub function is parameter-less.
	//
	// Mutating. The gate's mandatory `paths_hint` is enforced by the dispatcher
	// BEFORE the handler runs.
	Registry.Register(
		TEXT("unreal_open_mcp_blueprint_add_function"),
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
					TEXT("'path' is required (Blueprint asset object path)."));
			}

			UBlueprint* Blueprint = FUnrealOpenMcpBlueprintHelpers::ResolveBlueprint(Path);
			if (!Blueprint)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("blueprint_not_found"),
					FString::Printf(TEXT("Blueprint not found at '%s'."), *Path));
			}

			// Mutators must refuse the engine/script/temp content roots: the
			// mutation below dirties the Blueprint's package even when the caller
			// passes save:false. Only blueprint_create used to check a root, so
			// every other Blueprint mutator could edit /Engine content.
			FString BlueprintPackageName;
			if (!IsBlueprintInWritableRoot(Blueprint, BlueprintPackageName))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_content_root"),
					FString::Printf(
						TEXT("Refusing to modify '%s' under a reserved content root; ")
						TEXT("use a project root like '/Game'."),
						*BlueprintPackageName));
			}

			const FString FuncName = Args->HasTypedField<EJson::String>(TEXT("name"))
				? Args->GetStringField(TEXT("name"))
				: FString();
			if (FuncName.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'name' is required (new function graph name)."));
			}
			FString NameError;
			if (!IsNameWellFormed(Blueprint, FuncName, NameError))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("invalid_name"), NameError);
			}

			// Function-graph name collision — FunctionGraphs holds the user
			// function graphs blueprint_get reports. A duplicate name is a
			// structured refusal, never a silent overwrite.
			for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				if (Graph && Graph->GetFName() == FName(*FuncName))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("name_collision"),
						FString::Printf(TEXT("a function named '%s' already exists."), *FuncName));
				}
			}

			// Outer-name hijack guard. CreateNewGraph resolves a name clash by
			// RENAMING the existing object aside, so a name colliding with the
			// EventGraph or any other UObject outered to the Blueprint would
			// silently hijack it and report success. Probe ANY UObject on the
			// Blueprint's outer scope up front so the structured name_collision
			// fires instead of a silent rename-aside.
			if (FindObject<UObject>(Blueprint, *FuncName) != nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("name_collision"),
					FString::Printf(TEXT("a graph or object named '%s' already exists on this Blueprint."), *FuncName));
			}

			UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint, FName(*FuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (!NewGraph)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("create_graph_failed"),
					TEXT("FBlueprintEditorUtils::CreateNewGraph returned null."));
			}

			// SignatureFromObject = null → parameter-less stub. bIsUserCreated =
			// true so the function shows up under the user function list and is
			// callable from other graphs / C++ via the generated class.
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(
				Blueprint, NewGraph, /*bIsUserCreated*/ true, /*SignatureFromObject*/ static_cast<UClass*>(nullptr));

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

			UE_LOG(
				LogUnrealOpenMcp, Log,
				TEXT("[Unreal Open MCP] blueprint_add_function: '%s' on '%s' (stub — body authoring is out of scope)."),
				*FuncName, *Blueprint->GetName());

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("function"), FuncName);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Out)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// =========================================================================
	// unreal_open_mcp_blueprint_add_event — overridable parent event node.
	// =========================================================================
	//
	// `path` is the Blueprint asset object path; `name` is the parent UFunction
	// name of an overridable event (e.g. ReceiveBeginPlay / ReceiveTick). Adds an
	// event node to the event graph so the override can be authored + compiled.
	//
	// The two-pronged resolution mirrors the K2 editor's own behavior:
	//   1. A fresh Actor event graph is pre-seeded with DISABLED ghost nodes for
	//      the common events (ReceiveBeginPlay/ReceiveTick). AddDefaultEventNode
	//      would return that ghost rather than minting a second node — and the
	//      ghost is INERT (the event does NOT fire) until enabled. So an existing
	//      disabled ghost is exactly the node we want to ENABLE: enabling the
	//      ghost IS the "add event" operation.
	//   2. An ENABLED existing node is a real duplicate → reject.
	//   3. No existing node → AddDefaultEventNode mints a fresh one + the tool
	//      enables it.
	//
	// Guards (each maps to a structured error code):
	//   - Blueprint has no event graph      → no_event_graph
	//   - name names no parent UFunction    → not_a_function
	//   - name names a function that is NOT
	//     an overridable Blueprint event    → not_an_event
	//     (FindFunctionByName matches ANY parent UFunction, e.g. K2_DestroyActor
	//     is BlueprintCallable but not a BlueprintEvent; the schema's canonical
	//     check requires FUNC_BlueprintEvent and excludes static/const/deprecated/
	//     thread-safe functions — otherwise we'd seed a nonsense node + report
	//     success)
	//   - an enabled node for the event
	//     already exists                     → event_already_exists
	//   - AddDefaultEventNode returns null   → create_node_failed
	//
	// Body authoring is OUT OF SCOPE — this enables or creates an overridable
	// event node only. MarkBlueprintAsStructurallyModified follows so a later
	// compile wires the override.
	//
	// Mutating. The gate's mandatory `paths_hint` is enforced by the dispatcher
	// BEFORE the handler runs.
	Registry.Register(
		TEXT("unreal_open_mcp_blueprint_add_event"),
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
					TEXT("'path' is required (Blueprint asset object path)."));
			}

			UBlueprint* Blueprint = FUnrealOpenMcpBlueprintHelpers::ResolveBlueprint(Path);
			if (!Blueprint)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("blueprint_not_found"),
					FString::Printf(TEXT("Blueprint not found at '%s'."), *Path));
			}

			// Mutators must refuse the engine/script/temp content roots: the
			// mutation below dirties the Blueprint's package even when the caller
			// passes save:false. Only blueprint_create used to check a root, so
			// every other Blueprint mutator could edit /Engine content.
			FString BlueprintPackageName;
			if (!IsBlueprintInWritableRoot(Blueprint, BlueprintPackageName))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_content_root"),
					FString::Printf(
						TEXT("Refusing to modify '%s' under a reserved content root; ")
						TEXT("use a project root like '/Game'."),
						*BlueprintPackageName));
			}

			if (!Blueprint->ParentClass)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("not_an_event"),
					FString::Printf(TEXT("'%s' has no parent class, so no parent events can be overridden."), *Blueprint->GetName()));
			}

			UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
			if (!EventGraph)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_event_graph"),
					FString::Printf(TEXT("'%s' has no event graph."), *Blueprint->GetName()));
			}

			const FString EventName = Args->HasTypedField<EJson::String>(TEXT("name"))
				? Args->GetStringField(TEXT("name"))
				: FString();
			if (EventName.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'name' is required (parent event function name, e.g. 'ReceiveBeginPlay')."));
			}

			UFunction* EventFunc = Blueprint->ParentClass->FindFunctionByName(FName(*EventName));
			if (!EventFunc)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("not_an_event"),
					FString::Printf(TEXT("'%s' is not a function on parent class '%s' (overridable events are named e.g. ReceiveBeginPlay/ReceiveTick)."), *EventName, *Blueprint->ParentClass->GetName()));
			}

			// FunctionCanBePlacedAsEvent is the K2 schema's own canonical check:
			// it requires FUNC_BlueprintEvent and excludes static/const/deprecated/
			// thread-safe functions. FindFunctionByName alone would let a
			// BlueprintCallable-but-not-BlueprintEvent function (K2_DestroyActor)
			// through, seeding a nonsense node + reporting success.
			if (!UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(EventFunc))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("not_an_event"),
					FString::Printf(TEXT("'%s' is not an overridable Blueprint event on '%s' (the K2 schema rejected it — parent events are named e.g. ReceiveBeginPlay/ReceiveTick)."), *EventName, *Blueprint->ParentClass->GetName()));
			}

			// Two-pronged resolution. A fresh Actor event graph is pre-seeded
			// with DISABLED ghost nodes for the common events; FindOverrideFor-
			// Function returns that ghost. An ENABLED node is a real duplicate
			// (reject); a disabled ghost is exactly the node we want to ENABLE —
			// enabling the ghost IS the "add event" operation. No existing node
			// → AddDefaultEventNode mints a fresh one.
			UK2Node_Event* EventNode = FBlueprintEditorUtils::FindOverrideForFunction(
				Blueprint, EventFunc->GetOwnerClass(), FName(*EventName));
			if (EventNode && EventNode->IsNodeEnabled())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("event_already_exists"),
					FString::Printf(TEXT("event '%s' already exists on '%s'."), *EventName, *Blueprint->GetName()));
			}
			if (!EventNode)
			{
				int32 NodePosY = 0;
				EventNode = FKismetEditorUtilities::AddDefaultEventNode(
					Blueprint, EventGraph, FName(*EventName), EventFunc->GetOwnerClass(), NodePosY);
			}
			if (!EventNode)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("create_node_failed"),
					FString::Printf(TEXT("could not add event node for '%s'."), *EventName));
			}

			// The auto-placed ghost node is EnabledState=Disabled and excluded
			// from compilation; enable it so the event actually fires. bUserAction
			// false: programmatic enable, not a click in the graph editor.
			EventNode->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction*/ false);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

			UE_LOG(
				LogUnrealOpenMcp, Log,
				TEXT("[Unreal Open MCP] blueprint_add_event: '%s' enabled on '%s' (stub — body authoring is out of scope)."),
				*EventName, *Blueprint->GetName());

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("event"), EventName);
			Out->SetBoolField(TEXT("enabled"), true);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Out)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// =========================================================================
	// unreal_open_mcp_blueprint_compile — compile a Blueprint and return a
	// STRUCTURED error/warning list (the AI feedback loop).
	// =========================================================================
	//
	// `path` is the Blueprint asset object path. The handler runs
	// FKismetEditorUtilities::CompileBlueprint with a silent
	// FCompilerResultsLog (bSilentMode = true so the Output Log is not
	// flooded), then walks Results.Messages and maps each FTokenizedMessage to
	// { severity, message, node, graph }. The `node` / `graph` attribution is
	// best-effort: it is populated only when the compiler attached UObject
	// tokens pointing at a UEdGraphNode / UEdGraph (a missing field is the
	// common case for messages with no graph context).
	//
	// A FAILED compile is a NORMAL, expected result for the AI loop — it is NOT
	// a transport failure. The envelope stays `ok:true` (the dispatch itself
	// succeeded) and the result object carries `succeeded:false` + the
	// populated `messages[]` so an agent reads the diagnostics, fixes the
	// structure via the add/modify tools, and recompiles. Only TOOL-LEVEL
	// errors (malformed body, missing path, missing asset, reserved root) map
	// to `ok:false` / structured codes. This is the key contract that lets an
	// agent treat "compile failed" as data, not as an opaque isError.
	//
	// Mutating. The gate's mandatory `paths_hint` is enforced by the dispatcher
	// BEFORE the handler runs. Structured errors:
	//   - invalid_parameter          — malformed body
	//   - missing_parameter          — `path` absent
	//   - blueprint_not_found        — no Blueprint at path
	//   - invalid_content_root       — Blueprint under /Engine, /Script, /Temp
	//
	// (A non-zero error count from the compiler is NOT a structured error — it
	// rides through as `succeeded:false` on an `ok:true` envelope.)
	Registry.Register(
		TEXT("unreal_open_mcp_blueprint_compile"),
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
					TEXT("'path' is required (Blueprint asset object path)."));
			}

			UBlueprint* Blueprint = FUnrealOpenMcpBlueprintHelpers::ResolveBlueprint(Path);
			if (!Blueprint)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("blueprint_not_found"),
					FString::Printf(TEXT("Blueprint not found at '%s'."), *Path));
			}

			// Compile mutates the generated class + bytecode and dirties the
			// package, so the reserved-root refuse applies for the same reason
			// as every other Blueprint mutator (even a save:false call dirties
			// the package).
			FString BlueprintPackageName;
			if (!IsBlueprintInWritableRoot(Blueprint, BlueprintPackageName))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_content_root"),
					FString::Printf(
						TEXT("Refusing to modify '%s' under a reserved content root; ")
						TEXT("use a project root like '/Game'."),
						*BlueprintPackageName));
			}

			// bSilentMode = true: the compiler's own message pump would otherwise
			// echo every diagnostic to the editor Output Log on each compile —
			// noisy and slow in an agent recompile loop. The diagnostics still
			// land in Results.Messages for structured extraction below.
			FCompilerResultsLog Results;
			Results.bSilentMode = true;
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);

			// Walk the tokenized message log and map each entry to the agent-
			// consumable shape. severity collapses the EMessageSeverity enum to
			// the three tokens an agent switches on (error | warning | info).
			// node/graph attribution is best-effort: only populated when the
			// compiler attached a UObject token pointing at a UEdGraphNode /
			// UEdGraph (a message with no graph context leaves both empty — that
			// is the common case for type/property-level diagnostics).
			TArray<TSharedPtr<FJsonValue>> Messages;
			for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
			{
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				const EMessageSeverity::Type Sev = Msg->GetSeverity();
				Entry->SetStringField(TEXT("severity"),
					Sev == EMessageSeverity::Error ? TEXT("error") :
					Sev == EMessageSeverity::Warning ? TEXT("warning") : TEXT("info"));
				Entry->SetStringField(TEXT("message"), Msg->ToText().ToString());

				FString NodeName;
				FString GraphName;
				for (const TSharedRef<IMessageToken>& Token : Msg->GetMessageTokens())
				{
					if (Token->GetType() != EMessageToken::Object)
					{
						continue;
					}
					const FUObjectToken& ObjToken = static_cast<const FUObjectToken&>(Token.Get());
					const UObject* Obj = ObjToken.GetObject().Get();
					if (!Obj)
					{
						continue;
					}
					if (NodeName.IsEmpty() && Obj->IsA(UEdGraphNode::StaticClass()))
					{
						NodeName = Obj->GetName();
					}
					else if (GraphName.IsEmpty() && Obj->IsA(UEdGraph::StaticClass()))
					{
						GraphName = Obj->GetName();
					}
				}
				Entry->SetStringField(TEXT("node"), NodeName);
				Entry->SetStringField(TEXT("graph"), GraphName);
				Messages.Add(MakeShared<FJsonValueObject>(Entry));
			}

			// succeeded reflects the compiler's error count, NOT the dispatch.
			// An ok:true envelope with succeeded:false is exactly the "compile
			// failed — here are the diagnostics" contract the AI loop relies on.
			const bool bSucceeded = (Results.NumErrors == 0);
			const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("succeeded"), bSucceeded);
			Out->SetNumberField(TEXT("numErrors"), Results.NumErrors);
			Out->SetNumberField(TEXT("numWarnings"), Results.NumWarnings);
			Out->SetArrayField(TEXT("messages"), Messages);

			UE_LOG(
				LogUnrealOpenMcp, Log,
				TEXT("[Unreal Open MCP] blueprint_compile: '%s' %s (%d error(s), %d warning(s))."),
				*Blueprint->GetName(),
				bSucceeded ? TEXT("succeeded") : TEXT("failed"),
				Results.NumErrors, Results.NumWarnings);

			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Out)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// =========================================================================
	// unreal_open_mcp_blueprint_spawn — instance a compiled Actor Blueprint's
	// GeneratedClass into the current editor level.
	// =========================================================================
	//
	// `path` is the Blueprint asset object path (package-path form also accepted
	// via ResolveBlueprint's FindObject-first chain). The handler resolves the
	// Blueprint, requires a non-null `GeneratedClass` (the result of a compile —
	// an uncompiled-or-stale Blueprint has no spawnable class and is rejected
	// with `not_compiled` pointing the agent at blueprint_compile), and requires
	// that class to derive from AActor (a non-Actor Blueprint like a Blueprint
	// Function Library is rejected with `not_actor_blueprint`).
	//
	// HEADLESS-SAFE spawn path: `UWorld::SpawnActor` is used, NOT the viewport-
	// aware `UEditorActorSubsystem::SpawnActorFromClass`. The editor subsystem
	// touches the active 3D viewport and crashes under `-nullrhi` / Automation
	// (no viewport, no RHI), so the plain world spawn is the only path the
	// editor spec harness can exercise and the only one safe under a headless
	// editor. The world comes from `GEditor->GetEditorWorldContext().World()`
	// (resolved via the shared FUnrealOpenMcpObjectRef::GetEditorWorld helper
	// the actor family uses); no editor world → `no_editor_world`.
	//
	// Optional args:
	//   - `location` — `{x,y,z}` world location, default `{0,0,0}` (origin).
	//   - `name`     — actor label (SetActorLabelUnique so a colliding label is
	//                  de-duplicated and stays unambiguous to actor lookups).
	//
	// MVP scope: rotation is fixed at identity. No PIE-only spawn path, no
	// multiplayer, no parent attachment (use actor_create / actor_set_parent
	// for those). The spawn is wrapped in FScopedTransaction for editor Undo
	// and marks the level package dirty so the editor's save prompt fires.
	//
	// Mutating. The gate's mandatory `paths_hint` is enforced by the dispatcher
	// BEFORE the handler runs. Structured errors:
	//   - invalid_parameter       — malformed body
	//   - missing_parameter       — `path` absent
	//   - blueprint_not_found     — no Blueprint at path
	//   - not_compiled            — GeneratedClass missing (compile first)
	//   - not_actor_blueprint     — GeneratedClass is not AActor-derived
	//   - no_editor_world         — no GEditor / editor world
	//   - spawn_failed            — SpawnActor returned null
	Registry.Register(
		TEXT("unreal_open_mcp_blueprint_spawn"),
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
					TEXT("'path' is required (Blueprint asset object path)."));
			}

			UBlueprint* Blueprint = FUnrealOpenMcpBlueprintHelpers::ResolveBlueprint(Path);
			if (!Blueprint)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("blueprint_not_found"),
					FString::Printf(TEXT("Blueprint not found at '%s'."), *Path));
			}

			// Require a compiled GeneratedClass. A freshly-created Blueprint may
			// have a null GeneratedClass until the first compile; spawn on that
			// would dereference null, so surface a structured error that points
			// the agent at blueprint_compile instead of a native crash.
			UClass* GeneratedClass = Blueprint->GeneratedClass;
			if (GeneratedClass == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("not_compiled"),
					FString::Printf(
						TEXT("Blueprint '%s' has no GeneratedClass — run blueprint_compile first."),
						*Blueprint->GetName()));
			}

			// Only Actor Blueprints can be spawned. A Blueprint Function Library
			// / Object Blueprint compiles to a non-AActor generated class; the
			// explicit check turns the otherwise-silent SpawnActor null return
			// into a structured not_actor_blueprint.
			if (!GeneratedClass->IsChildOf(AActor::StaticClass()))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("not_actor_blueprint"),
					FString::Printf(
						TEXT("Blueprint '%s' GeneratedClass is not an Actor class (only Actor Blueprints can be spawned)."),
						*Blueprint->GetName()));
			}

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			const FString Label = Args->HasTypedField<EJson::String>(TEXT("name"))
				? Args->GetStringField(TEXT("name"))
				: FString();
			const FVector Location = ReadVectorField(Args, TEXT("location"), FVector::ZeroVector);

			// FScopedTransaction opens the undo bracket; SpawnActor + the label
			// Modify() record into it. RF_Transactional on the spawn flags keeps
			// the new actor itself undo-trackable (matches the actor_create
			// pattern). ESPawnActorFlags here is the default set MINUS
			// `DeferredBegin` (the standard SpawnActor template default), which
			// is what the behavior reference (Unreal-MCP blueprint-spawn) passes
			// — plain spawn, no special collision/script init flags.
			const FScopedTransaction Transaction(
				NSLOCTEXT("UnrealOpenMcp", "BlueprintSpawn", "MCP: Spawn Blueprint"));
			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transactional;
			SpawnParams.bDeferConstruction = false;
			// AlwaysSpawn: skip the editor's try-to-reuse-instance path so a
			// spawn request always yields a fresh actor (matches actor_create).
			SpawnParams.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

			AActor* Actor = World->SpawnActor<AActor>(
				GeneratedClass, Location, FRotator::ZeroRotator, SpawnParams);
			if (Actor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("spawn_failed"),
					FString::Printf(
						TEXT("SpawnActor returned null for Blueprint '%s' (class '%s')."),
						*Blueprint->GetName(),
						*GeneratedClass->GetName()));
			}

			// SetActorLabelUnique (not SetActorLabel): a user-supplied label
			// colliding with an existing actor's label would make both ambiguous
			// to ResolveActor. Empty label → keep the engine default.
			if (!Label.IsEmpty())
			{
				FActorLabelUtilities::SetActorLabelUnique(Actor, Label);
			}

			// Mark the level package dirty so the editor's save prompt fires.
			// The outer of a spawned actor is its ULevel; MarkPackageDirty on
			// the level propagates the dirty bit the editor keys off.
			if (ULevel* Level = Actor->GetLevel())
			{
				Level->MarkPackageDirty();
			}

			// Minimal actor identity for the agent: label + class + path + the
			// spawned transform's location. Deliberately lighter than actor_create's
			// full ActorData — spawn is the last step of the compile loop, the
			// agent already knows the Blueprint's structure from blueprint_get.
			const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("actor"), Actor->GetActorLabel());
			Out->SetStringField(TEXT("name"), Actor->GetName());
			Out->SetStringField(TEXT("class"),
				Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
			Out->SetStringField(TEXT("path"), Actor->GetPathName());

			const TSharedRef<FJsonObject> LocJson = MakeShared<FJsonObject>();
			LocJson->SetNumberField(TEXT("x"), Actor->GetActorLocation().X);
			LocJson->SetNumberField(TEXT("y"), Actor->GetActorLocation().Y);
			LocJson->SetNumberField(TEXT("z"), Actor->GetActorLocation().Z);
			Out->SetObjectField(TEXT("location"), LocJson);

			UE_LOG(
				LogUnrealOpenMcp, Log,
				TEXT("[Unreal Open MCP] blueprint_spawn: '%s' -> actor '%s' (class %s) at %s."),
				*Blueprint->GetName(),
				*Actor->GetActorLabel(),
				Actor->GetClass() ? *Actor->GetClass()->GetName() : TEXT("?"),
				*Actor->GetActorLocation().ToString());

			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Out)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	UE_LOG(
		LogUnrealOpenMcp, Log,
		TEXT("[Unreal Open MCP] blueprint tools registered: unreal_open_mcp_blueprint_create, unreal_open_mcp_blueprint_get, unreal_open_mcp_blueprint_add_component, unreal_open_mcp_blueprint_remove_component, unreal_open_mcp_blueprint_add_variable, unreal_open_mcp_blueprint_modify_variable, unreal_open_mcp_blueprint_set_default, unreal_open_mcp_blueprint_add_function, unreal_open_mcp_blueprint_add_event, unreal_open_mcp_blueprint_compile, unreal_open_mcp_blueprint_spawn"));
}
