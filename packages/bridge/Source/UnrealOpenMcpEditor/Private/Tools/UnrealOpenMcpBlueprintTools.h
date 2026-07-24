// Blueprint-tool family registration for the bridge tool surface.
//
// This file is the Blueprint family SPINE: the shared helpers it exposes
// (ResolveBlueprint, path normalization, name well-formedness, BlueprintRef
// JSON, pin-type reverse mapping) are reused by every later P6 sub-plan
// (variables, components, graph authoring, compile, spawn). create + get land
// here first.
//   - `unreal_open_mcp_blueprint_create` — create a new Blueprint class from a
//     parent UClass via the public FKismetEditorUtilities::CreateBlueprint.
//     `path` is the /Game package path for the asset (e.g.
//     '/Game/Mcp/BP_Thing'); the object-path form ('/Game/Mcp/BP_Thing.BP_Thing')
//     is also accepted and normalised. `parent_class` defaults to Actor
//     ('/Script/Engine.Actor'); other Blueprintable parents allowed when
//     CanCreateBlueprintOfClass passes. The asset is registered in-session
//     (AssetRegistry.AssetCreated + MarkPackageDirty); no disk save. A
//     collision with ANY UObject at the target path (not just a Blueprint) is
//     probed up front so the engine's fatal "same fully-qualified name,
//     different class" allocation check is never hit.
//   - `unreal_open_mcp_blueprint_get` — read-only scoped graph summary for LLM
//     inspection: member variables (name/type/array), SCS components
//     (name/class/parent), functions and events with node-counts + an `enabled`
//     flag on events, implemented interfaces, and the parent class chain.
//   - `unreal_open_mcp_blueprint_add_component` — add a node to the Blueprint's
//     Simple Construction Script via the public SCS surface (CreateNode +
//     AddChildNode under an optional scene-component parent, or AddNode for a
//     root). Guards: the component class must be a non-abstract, non-deprecated
//     UActorComponent subclass (SCS CreateNode -> NewObject fatally asserts on
//     an abstract class, e.g. '/Script/Engine.LightComponentBase', so the
//     ClassFlags check runs BEFORE CreateNode); the variable name must not
//     collide across the SCS, the member-variable list, or the parent class's
//     properties (those namespaces share the generated class's property
//     namespace and would otherwise only fail at compile); and attachment is a
//     scene-graph op so both the new component and its parent must be
//     USceneComponents. MarkBlueprintAsStructurallyModified so a later compile
//     rebuilds the CDO.
//   - `unreal_open_mcp_blueprint_remove_component` — delete an SCS node by
//     variable name via RemoveNodeAndPromoteChildren (children re-parented onto
//     the removed node's parent so a subtree is never orphaned) +
//     MarkBlueprintAsStructurallyModified.
//
// create / add_component / remove_component are MUTATING and register with
// `FUnrealOpenMcpToolMetadata::Mutating()` so the dispatcher wraps them in
// `GatePolicy.Execute` (the mandatory `paths_hint` is enforced by the
// dispatcher BEFORE the handler runs). get is read-only (gate Off).
//
// Fidelity: greenfield. There is no Unity Blueprint / prefab-graph twin — the
// Unity-first porting protocol still applies to shared infrastructure (gate
// contract, snake_case naming, MCP envelope), but the create/get + SCS add/remove
// surface is Unreal-only. Behavior reference (read-only): Unreal-MCP's
// blueprint-create / blueprint-get / blueprint-add-component /
// blueprint-remove-component handlers + ResolveBlueprint / BlueprintRefStruct /
// name helpers (UnrealMcpBlueprintTools.cpp) for the correct Kismet editor API
// usage (CanCreateBlueprintOfClass / CreateBlueprint / AssetCreated / the
// any-UObject collision probe / the disabled-ghost-event `enabled` flag / the
// abstract-class ClassFlags guard / the cross-namespace name-collision checks /
// the USceneComponent attachment validation / RemoveNodeAndPromoteChildren).
//
// Every handler registered here runs ON THE GAME THREAD (the HTTP server
// marshals dispatch through the GameThreadDispatcher).
#pragma once

#include "CoreMinimal.h"

class FUnrealOpenMcpToolRegistry;
class UBlueprint;
struct FEdGraphPinType;

/**
 * Register the Blueprint-tool family with @p Registry. First-registration-wins:
 * a duplicate name is ignored by the registry.
 *
 * Registers:
 *   - `unreal_open_mcp_blueprint_create`         (mutating; gate Enforce; paths_hint required)
 *   - `unreal_open_mcp_blueprint_get`            (read-only)
 *   - `unreal_open_mcp_blueprint_add_component`    (mutating; gate Enforce; paths_hint required)
 *   - `unreal_open_mcp_blueprint_remove_component` (mutating; gate Enforce; paths_hint required)
 */
namespace FUnrealOpenMcpBlueprintTools
{
	void Register(FUnrealOpenMcpToolRegistry& Registry);
}

/**
 * Shared Blueprint helpers reused by every later P6 sub-plan (variables,
 * components, graph authoring, compile, spawn). Lives in the header so a later
 * tool translation unit can resolve/inspect a Blueprint without re-implementing
 * the resolve chain.
 *
 * Behavior reference (read-only): Unreal-MCP's ResolveBlueprint +
 * BlueprintRefStruct + PinTypeToString (UnrealMcpBlueprintTools.cpp).
 */
namespace FUnrealOpenMcpBlueprintHelpers
{
	/**
	 * Resolve a UBlueprint by object path. Prefers an already-in-memory object
	 * (assets created earlier this session are registered with the AssetRegistry
	 * but not yet saved to disk), then falls back to load. Returns null for an
	 * empty path or when nothing matches.
	 *
	 * LOAD_NoWarn | LOAD_Quiet on the fallback: a missing asset is an expected
	 * outcome (the caller turns it into a structured "not found" error); without
	 * the flags the speculative load warn-spams the editor log.
	 */
	UNREALOPENMCPEDITOR_API UBlueprint* ResolveBlueprint(const FString& Path);

	/**
	 * Map an FEdGraphPinType to a friendly type string for the blueprint_get
	 * variables surface (reverse of the forward mapping P6.3 will land for
	 * variable add). Primitives + common math structs collapse to short tokens
	 * ('bool', 'vector', ...); struct/object paths use the sub-category object's
	 * name; containers append '[]' / '{}' / '<map>'. Shared here so P6.3's
	 * variable-add result round-trips through the same string space.
	 */
	UNREALOPENMCPEDITOR_API FString PinTypeToString(const FEdGraphPinType& PinType);
}
