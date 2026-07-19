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
// Both tools are read-only (route live; gate Off). They carry no `paths_hint`
// surface — that arrives with the mutating asset tools.
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
// Behavior reference (read-only): Unreal-MCP's asset handlers
// (UnrealMcpAssetTools.cpp — asset-find / asset-get-data). The filter-shape
// (PackagePaths + ClassPaths + recursive flags + tag filter), the
// short-name substring post-filter (AssetRegistry has no substring
// primitive), the deterministic object-path sort, the empty-filter → /Game
// default, the `/` + `.` class-path validation BEFORE FTopLevelAssetPath
// construction (avoids the engine ensure), the AssetDataToJson shape, and
// the TagsAndValues.ForEach serialization were studied for correct Unreal
// editor API usage and adapted to this port's Ok/Fail result shape.
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
 * Registers: `unreal_open_mcp_asset_find` (read-only),
 *             `unreal_open_mcp_asset_get_data` (read-only).
 */
namespace FUnrealOpenMcpAssetTools
{
	void Register(FUnrealOpenMcpToolRegistry& Registry);
}
