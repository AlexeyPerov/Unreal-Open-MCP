// Blueprint-tool family — see header for the create/get contracts.
// This file owns the two handlers (blueprint_create, blueprint_get) plus the
// shared helpers the later P6 sub-plans reuse (ResolveBlueprint, path
// normalization, name well-formedness, BlueprintRef JSON, pin-type reverse
// mapping).
//
// Arg parsing + output mirror the material / asset families: each handler
// parses the raw POST body into an FJsonObject and emits a pre-serialized JSON
// string handed to FUnrealOpenMcpToolDispatchResult::Output. The registry/
// dispatch spine stays raw-body — only the handler layer parses.
//
// Behavior reference (read-only): Unreal-MCP's blueprint-create / blueprint-get
// (UnrealMcpBlueprintTools.cpp). The any-UObject collision probe (so a
// non-Blueprint asset at the target path does not trip the engine's fatal
// "same fully-qualified name, different class" allocation check), the
// MarkAsGarbage cleanup on a failed create, the AssetRegistry.AssetCreated +
// MarkPackageDirty registration, and the disabled-ghost-event `enabled` flag
// were studied for correct Kismet editor API usage and adapted to this port's
// Ok/Fail result shape.
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

#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
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
		else if (Lower == TEXT("float") || Lower == TEXT("double") || Lower == TEXT("number") || Lower == TEXT("real"))
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
	else if (Cat == UEdGraphSchema_K2::PC_Real) Base = TEXT("float"); // float/double both map to PC_Real
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
			// Attachment is a scene-graph operation: both the new component and
			// its parent must be USceneComponents, otherwise AddChildNode
			// produces an invalid (non-attachable) hierarchy.
			if (ParentNode)
			{
				if (!CompClass->IsChildOf(USceneComponent::StaticClass()))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_attachment"),
						FString::Printf(TEXT("'%s' is not a USceneComponent, so it cannot be attached under a parent component."), *CompClass->GetName()));
				}
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
					TEXT("no_generated_class"),
					TEXT("Could not resolve the Class Default Object."));
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

	UE_LOG(
		LogUnrealOpenMcp, Log,
		TEXT("[Unreal Open MCP] blueprint tools registered: unreal_open_mcp_blueprint_create, unreal_open_mcp_blueprint_get, unreal_open_mcp_blueprint_add_component, unreal_open_mcp_blueprint_remove_component, unreal_open_mcp_blueprint_add_variable, unreal_open_mcp_blueprint_modify_variable, unreal_open_mcp_blueprint_set_default"));
}
