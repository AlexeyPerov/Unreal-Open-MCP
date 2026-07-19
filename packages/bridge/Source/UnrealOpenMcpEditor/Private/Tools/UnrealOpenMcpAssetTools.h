// Asset-tool family registration for the bridge tool surface.
//
// P4.1 ships the AssetRegistry read spine — the Unreal analog of Unity Open
// MCP's search_assets / read_asset family, adapted to Unreal's live
// AssetRegistry model (ADR-006: live-only, no offline .uasset parse):
//   - `unreal_open_mcp_asset_find` — filtered AssetRegistry query with stable
//     ordering and offset/limit pagination. Empty filter defaults to /Game
//     (never the whole registry incl. /Engine by accident).
//   - `unreal_open_mcp_asset_get_data` — single-asset metadata read: name,
//     object path, package, class, and the registry tag map.
//
// P4.2 adds the Content Browser mutators — the Unreal analog of Unity Open
// MCP's assets-create-folder / copy / move / delete / refresh family,
// adapted to Unreal's live EditorAssetLibrary + AssetRegistry scan model:
//   - `unreal_open_mcp_asset_create_folder` — MakeDirectory, idempotent
//     (returns `created: false` when the folder already exists).
//   - `unreal_open_mcp_asset_copy` — DuplicateAsset; refuses if the
//     destination already exists (no silent overwrite).
//   - `unreal_open_mcp_asset_move` — RenameAsset; refuses if the destination
//     already exists; a redirector may remain at the source path.
//   - `unreal_open_mcp_asset_delete` — DeleteAsset; REFUSES when other
//     packages reference the asset unless `force: true` is passed (the
//     registry referencer list is returned so the agent can decide).
//   - `unreal_open_mcp_asset_refresh` — IAssetRegistry::ScanPathsSynchronous.
//     Classified read-only: refresh reschedules discovery state without
//     touching on-disk packages or UObject graph, so the gate would have
//     nothing to checkpoint (matches the Unity `assets_refresh` semantics
//     after re-reading the Unity metadata — Unity also classifies refresh
//     as mutating, but here the gate has no on-disk diff to compute and
//     every P4.2 mutation that needs a gate already creates/copies/moves
//     /deletes the package directly; refresh is just registry bookkeeping).
//
// This file also owns the shared helpers the later P4 mutators (CRUD,
// materials, import) build on:
//   - `GetAssetRegistry()` — `IAssetRegistry&` from the AssetRegistry module.
//   - `NormalizeContentPath` — collapse object-path vs package-path forms to
//     the long package path the AssetRegistry keys off.
//   - `AssetDataToJson(FAssetData)` — the registry metadata block shared by
//     find + get-data (and later list-tools).
//   - `IsWritableContentRoot(Path)` — the writable-root predicate used by
//     P4.2–P4.4 to refuse engine content roots on writes. Defined here so
//     the rule lives next to the path helpers; unused by find/get which are
//     pure reads.
//
// P4.1 tools are read-only (route live; gate Off). The P4.2 create_folder /
// copy / move / delete tools are MUTATING and register with
// `FUnrealOpenMcpToolMetadata::Mutating()` so the dispatcher wraps them in
// `GatePolicy.Execute`. `asset_refresh` is read-only by the same metadata
// classification as the find/get tools — see the per-tool comment in the
// .cpp for the reasoning.
//
// Adapted from Unity Open MCP's search-assets.ts / read-asset.ts (the agent
// workflow: find → drill-down) at adapt fidelity:
//   - Unity scans Assets/ on disk (offline-first); Unreal P4 is live-only
//     (ADR-006). No disk .uasset parse.
//   - Naming follows the Unreal-MCP style (`asset_find` / `asset_get_data`),
//     not Unity's `search_assets` / `read_asset`.
//   - Path roots are `/Game/...`, not `Assets/`.
//   - Pagination starts with offset/limit (Unreal-MCP); Unity's
//     profile/cursor axis can layer later if payloads demand it.
//   - get-data returns registry metadata + tags, not a full property dump
//     (property drill-down stays on actor/object tools / later work).
//
// Adapted from Unity Open MCP's assets-create-folder.ts / assets-copy.ts /
// assets-move.ts / assets-delete.ts / assets-refresh.ts (P4.2) at adapt
// fidelity:
//   - Single path per call (`path`/`source`/`destination`) replaces Unity's
//     `folders[]` / `entries[]` batch arrays. Unreal-MCP ships single-path
//     tools; batching can layer later if a workflow needs it.
//   - Writable-root guard (`IsWritableContentRoot`) replaces Unity's
//     Assets/-rooted path check — /Engine, /Script, /Temp are refused on
//     writes so engine content cannot be silently corrupted.
//   - delete surfaces referencers via AssetRegistry::GetReferencers instead
//     of Unity's missing-script delete surface; `force: true` overrides.
//   - refresh wraps `IAssetRegistry::ScanPathsSynchronous`, not Unity's
//     `AssetDatabase.Refresh()` — scoped to caller-supplied package paths.
//
// Behavior reference (read-only): Unreal-MCP's asset handlers
// (UnrealMcpAssetTools.cpp — asset-find / asset-get-data / asset-create-
// folder / asset-copy / asset-move / asset-delete / asset-refresh). The
// filter-shape (PackagePaths + ClassPaths + recursive flags + tag filter),
// the short-name substring post-filter (AssetRegistry has no substring
// primitive), the deterministic object-path sort, the empty-filter → /Game
// default, the `/` + `.` class-path validation BEFORE FTopLevelAssetPath
// construction (avoids the engine ensure), the AssetDataToJson shape, the
// TagsAndValues.ForEach serialization, the MakeDirectory idempotent shape
// (`created: false` when the folder exists), the DuplicateAsset /
// RenameAsset dest-already-exists refusal, the DeleteAsset referencer guard
// + `force` override, and the ScanPathsSynchronous rescan shape were studied
// for correct Unreal editor API usage and adapted to this port's Ok/Fail
// result shape.
//
// Every handler registered here runs ON THE GAME THREAD (the HTTP server
// marshals dispatch through the GameThreadDispatcher).
#pragma once

#include "CoreMinimal.h"

class FUnrealOpenMcpToolRegistry;

/**
 * Register the asset-tool family with @p Registry. Each asset tool is
 * registered here so the module boot wires the whole family in one place.
 * First-registration-wins: a duplicate name is ignored by the registry.
 *
 * Registers:
 *   - `unreal_open_mcp_asset_find`         (read-only)
 *   - `unreal_open_mcp_asset_get_data`     (read-only)
 *   - `unreal_open_mcp_asset_create_folder` (mutating)
 *   - `unreal_open_mcp_asset_copy`         (mutating)
 *   - `unreal_open_mcp_asset_move`         (mutating)
 *   - `unreal_open_mcp_asset_delete`       (mutating)
 *   - `unreal_open_mcp_asset_refresh`      (read-only)
 */
namespace FUnrealOpenMcpAssetTools
{
	void Register(FUnrealOpenMcpToolRegistry& Registry);
}
