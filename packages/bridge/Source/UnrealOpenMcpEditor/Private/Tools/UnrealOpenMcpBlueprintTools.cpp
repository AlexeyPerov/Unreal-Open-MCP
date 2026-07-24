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

#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node_Event.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackageName.h"

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

	UE_LOG(
		LogUnrealOpenMcp, Log,
		TEXT("[Unreal Open MCP] blueprint tools registered: unreal_open_mcp_blueprint_create, unreal_open_mcp_blueprint_get"));
}
