// Material-tool family registration for the bridge tool surface.
//
// P4.3 ships the typed material surface — the Unreal analog of Unity Open
// MCP's material-create / material-set-property / material-get-properties
// family, adapted to Unreal's UMaterialInstanceConstant + UMaterialEditingLibrary
// model (P4.3 plan: prefer MIC create/parameter edits over full material graph
// authoring):
//   - `unreal_open_mcp_material_create` — create a UMaterialInstanceConstant
//     from a parent UMaterialInterface via UMaterialInstanceConstantFactoryNew
//     + IAssetTools::CreateAsset. Refuses engine content roots and
//     destination-already-exists collisions (no silent overwrite).
//   - `unreal_open_mcp_material_modify` — apply scalar / vector / texture
//     parameter overrides on a UMaterialInstanceConstant. Requires at least one
//     of `scalars` / `vectors` / `textures`; rejects an empty no-op modify.
//     In-memory (marks the package dirty) unless `save: true` writes the
//     package to disk.
//   - `unreal_open_mcp_material_get_data` — read the scalar / vector / texture
//     parameter inventory + current values (instance override or base-material
//     default) plus the parent path. Read-only; supports the `paths` scoped
//     projection the asset family documents.
//
// create + modify are MUTATING and register with
// `FUnrealOpenMcpToolMetadata::Mutating()` so the dispatcher wraps them in
// `GatePolicy.Execute` (the mandatory `paths_hint` is enforced by the
// dispatcher BEFORE the handler runs). get-data is read-only (gate Off).
//
// Adapted from Unity Open MCP's material-create.ts / material-set-property.ts /
// material-get-properties.ts at adapt fidelity:
//   - Unity create takes a `shader_name` + writes a `.mat`; Unreal v1 creates a
//     UMaterialInstanceConstant from a parent material interface (shader swap /
//     material_set_shader deferred — not in the P4 roadmap).
//   - Unity's single-property material_set_property becomes batch scalars /
//     vectors / textures maps so an agent can set many params per call.
//   - Naming follows the roadmap `material_*` style (not Unreal-MCP's
//     `asset-material-*` kebab host names).
//
// Behavior reference: Unreal-MCP's asset-material-create / asset-material-modify
// / asset-material-get-data handlers (UnrealMcpAssetTools.cpp). The factory
// GC-root guard, the "engine setters always return false → validate names
// against the known parameter set" applied/failed classification, the
// partial-vector seed-from-current, the empty-modify transaction cancel (skip
// MarkPackageDirty), and the base-material default value fallback were studied
// for correct Unreal editor API usage and adapted to this port's Ok/Fail
// result shape.
//
// Every handler registered here runs ON THE GAME THREAD (the HTTP server
// marshals dispatch through the GameThreadDispatcher).
#pragma once

#include "CoreMinimal.h"

class FUnrealOpenMcpToolRegistry;

/**
 * Register the material-tool family with @p Registry. First-registration-wins:
 * a duplicate name is ignored by the registry.
 *
 * Registers:
 *   - `unreal_open_mcp_material_create`   (mutating)
 *   - `unreal_open_mcp_material_modify`   (mutating)
 *   - `unreal_open_mcp_material_get_data` (read-only)
 */
namespace FUnrealOpenMcpMaterialTools
{
	void Register(FUnrealOpenMcpToolRegistry& Registry);
}
