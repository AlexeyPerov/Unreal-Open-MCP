// Level-tool family registration for the bridge tool surface.
//
// P2.6 ships the level lifecycle family — the Unreal analog of Unity Open
// MCP's scene_* family, adapted to Unreal's .umap / streaming-level model:
//   - `unreal_open_mcp_level_open` — open an existing level by content path
//     (`FEditorFileUtils::LoadMap`), replacing the current editor world.
//   - `unreal_open_mcp_level_save` — save the current level in place, or
//     save-as the persistent level to a new path (save-as via `path`).
//   - `unreal_open_mcp_level_list_loaded` — enumerate the persistent level
//     + loaded streaming sublevels with path-first identity (read-only).
//   - `unreal_open_mcp_level_set_current` — set the current editing level by
//     short name or full package path (`UEditorLevelUtils::MakeLevelCurrent`).
//   - `unreal_open_mcp_level_unload_sublevel` — unload a loaded streaming
//     sublevel (`UEditorLevelUtils::RemoveLevelFromWorld`); the persistent
//     level cannot be unloaded.
//
// P2.7 adds two more level tools — the inspect + create pair:
//   - `unreal_open_mcp_level_get_data` — read-only actor roster of the current
//     editor world (or a loaded sublevel named by `path`), with a token-budget
//     profile (compact/balanced/full) + pagination (page_size/cursor). World
//     Partition levels surface `worldPartition:true` +
//     `partitionScope:"loaded-cells-only"` so an agent does not mistake a
//     sparse roster for the complete actor set.
//   - `unreal_open_mcp_level_create` — create a new, empty (or template-seeded)
//     level and make it the active editor world, optionally persisting it to a
//     content path. The Unreal analog of Unity's scene_create.
//
// The read-only tools (level_list_loaded, level_get_data) carry no gate; the
// mutators carry the forward-compat `paths_hint` + `gate` surface (no-op until
// P3.5, matching the actor family). level_open + level_create additionally
// honor a dirty guard (FUnrealOpenMcpLevelDirtyGuard): they refuse to replace
// a world with unsaved edits unless `ignore_dirty` is set, mirroring Unity's
// SceneDirtyGuard.
//
// Adapted from Unity Open MCP's ScenesTools
// (packages/bridge/Editor/TypedTools/ScenesTools.cs — scene-open / scene-save
// / scene-list-opened / scene-set-active / scene-unload / scene-get-data /
// scene-create) at adapt fidelity:
//   - Asset paths are Unreal content paths (`/Game/Maps/Arena`), not Unity's
//     `Assets/Scenes/Foo.unity`. The path may be a long package name or an
//     object path; both are normalised before the engine call.
//   - level_open / level_create REPLACE the world (no additive open in P2).
//     Unity's scene-open has a Single/Additive mode split; Unreal
//     additive/streaming is covered instead by level_set_current + a future
//     add-streaming tool.
//   - Dirty tracking is Unreal package dirty state
//     (UEditorLoadingAndSavingUtils::GetDirtyMapPackages), not Unity's
//     per-scene isDirty. There is no public "is level dirty" API like Godot's;
//     the engine's package dirty bit is the source of truth.
//   - No domain reload — Unreal map load does not trigger assembly reload, so
//     the Unity RestartThenSettle lifecycle metadata does not apply.
//
// Behavior reference (read-only): Unreal-MCP's level handlers
// (UnrealMcpLevelTools.cpp — level-create / open / save / list-loaded /
// set-current / unload-sublevel / level-get-data). The path-normalisation +
// content-aware existence probe (FPackageName::IsValidLongPackageName /
// DoesPackageExist + the .umap extension guard), the save-in-place transient
// guard, the short-name-vs-package-path disambiguation, the discardedDirtyLevels
// note, and the GEditor->NewMap / NewMapFromTemplate create surfaces were
// studied for correct Unreal editor API usage.
//
// Every handler registered here runs ON THE GAME THREAD (the HTTP server
// marshals dispatch through the GameThreadDispatcher).
#pragma once

#include "CoreMinimal.h"

class FUnrealOpenMcpToolRegistry;

/**
 * Register the level-tool family with @p Registry. Each level tool is
 * registered here so the module boot wires the whole family in one place.
 * First-registration-wins: a duplicate name is ignored by the registry.
 *
 * Registers: `unreal_open_mcp_level_open` (mutating; dirty guard; gate
 *             deferred),
 *             `unreal_open_mcp_level_save` (mutating; gate deferred),
 *             `unreal_open_mcp_level_list_loaded` (read-only),
 *             `unreal_open_mcp_level_set_current` (mutating; gate deferred),
 *             `unreal_open_mcp_level_unload_sublevel` (mutating; gate
 *             deferred),
 *             `unreal_open_mcp_level_get_data` (read-only; WP-aware),
 *             `unreal_open_mcp_level_create` (mutating; dirty guard; gate
 *             deferred).
 */
namespace FUnrealOpenMcpLevelTools
{
	void Register(FUnrealOpenMcpToolRegistry& Registry);
}
