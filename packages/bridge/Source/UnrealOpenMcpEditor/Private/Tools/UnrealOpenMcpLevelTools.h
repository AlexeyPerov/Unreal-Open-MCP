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
// The read-only tool (level_list_loaded) carries no gate; the four mutators
// carry the forward-compat `paths_hint` + `gate` surface (no-op until P3.5,
// matching the actor family). level_open additionally honors a dirty guard
// (FUnrealOpenMcpLevelDirtyGuard): it refuses to replace a world with unsaved
// edits unless `ignore_dirty` is set, mirroring Unity's SceneDirtyGuard.
//
// Adapted from Unity Open MCP's ScenesTools
// (packages/bridge/Editor/TypedTools/ScenesTools.cs — scene-open / scene-save
// / scene-list-opened / scene-set-active / scene-unload) at adapt fidelity:
//   - Asset paths are Unreal content paths (`/Game/Maps/Arena`), not Unity's
//     `Assets/Scenes/Foo.unity`. The path may be a long package name or an
//     object path; both are normalised before the engine call.
//   - level_open REPLACES the world (no additive open in P2). Unity's
//     scene-open has a Single/Additive mode split; Unreal additive/streaming
//     is covered instead by level_set_current + a future add-streaming tool.
//   - Dirty tracking is Unreal package dirty state
//     (UEditorLoadingAndSavingUtils::GetDirtyMapPackages), not Unity's
//     per-scene isDirty. There is no public "is level dirty" API like Godot's;
//     the engine's package dirty bit is the source of truth.
//   - No domain reload — Unreal map load does not trigger assembly reload, so
//     the Unity RestartThenSettle lifecycle metadata does not apply.
//
// Behavior reference (read-only): Unreal-MCP's level handlers
// (UnrealMcpLevelTools.cpp — level-create / open / save / list-loaded /
// set-current / unload-sublevel). The path-normalisation + content-aware
// existence probe (FPackageName::IsValidLongPackageName / DoesPackageExist +
// the .umap extension guard), the save-in-place transient guard, the
// short-name-vs-package-path disambiguation, and the discardedDirtyLevels note
// were studied for correct Unreal editor API usage.
//
// Every handler registered here runs ON THE GAME THREAD (the HTTP server
// marshals dispatch through the GameThreadDispatcher).
#pragma once

#include "CoreMinimal.h"

class FUnrealOpenMcpToolRegistry;

/**
 * Register the level-tool family with @p Registry. Each P2.6 level tool is
 * registered here so the module boot wires the whole family in one place.
 * First-registration-wins: a duplicate name is ignored by the registry.
 *
 * P2.6 registers: `unreal_open_mcp_level_open` (mutating; dirty guard; gate
 *                 deferred),
 *                 `unreal_open_mcp_level_save` (mutating; gate deferred),
 *                 `unreal_open_mcp_level_list_loaded` (read-only),
 *                 `unreal_open_mcp_level_set_current` (mutating; gate deferred),
 *                 `unreal_open_mcp_level_unload_sublevel` (mutating; gate
 *                 deferred).
 */
namespace FUnrealOpenMcpLevelTools
{
	void Register(FUnrealOpenMcpToolRegistry& Registry);
}
