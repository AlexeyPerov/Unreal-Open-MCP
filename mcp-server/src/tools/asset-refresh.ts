import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P4.2 — AssetRegistry refresh. Wraps IAssetRegistry::ScanPathsSynchronous so
// newly added files on disk become visible to the registry without an agent
// composing editor scripts. `paths` (array) defaults to ['/Game'] when
// omitted; a single-string `path` is also accepted (deduped with `paths`).
// `force: true` triggers a full blocking rescan (slow for large trees).
//
// Classification: READ-ONLY. Unlike Unity's AssetDatabase.Refresh (which can
// trigger an import/compile, hence Unity marks assets_refresh mutating with
// gate Enforce), Unreal's ScanPathsSynchronous only updates the in-memory
// AssetRegistry cache — it does not write packages or change the UObject
// graph. The gate would have no on-disk diff to checkpoint. Registered with
// the ReadOnly metadata so the dispatcher runs it directly (no gate).
//
// Intentional deltas vs Unity's assets_refresh:
//   - Read-only classification (Unity marks assets_refresh mutating because
//     AssetDatabase.Refresh can trigger recompile). Unreal's
//     ScanPathsSynchronous does not write packages or trigger Live Coding,
//     so there is no on-disk delta for the gate to checkpoint.
//   - `paths` (array of package paths) replaces Unity's `whole_project`
//     boolean — the Unreal API is scoped by path, and the empty-filter
//     default is /Game (never the whole registry incl. /Engine).
//   - No `paths_hint` / `gate` surface because the tool is read-only.
//
// Route: live (POST /tools/unreal_open_mcp_asset_refresh). Read-only.
export const assetRefresh: Tool = {
  name: "unreal_open_mcp_asset_refresh",
  description:
    "Refresh the AssetRegistry to pick up files added/removed/changed on " +
    "disk outside the editor. Use after direct filesystem edits (creating " +
    "folders, dropping textures, copying .uasset files into Content/). " +
    "Read-only (gate-free) — ScanPathsSynchronous only updates the " +
    "in-memory registry cache; it does not write packages or trigger Live " +
    "Coding. Pass `paths` (array of package paths, default ['/Game']) to " +
    "scope the rescan, or a single-string `path` (deduped with `paths`). " +
    "Pass `force: true` for a full blocking rescan (slow for large trees). " +
    "Result shape: { paths: string[], force: boolean }. Error codes: " +
    "invalid_parameter (malformed body or non-array `paths`).",
  inputSchema: {
    type: "object",
    properties: {
      paths: {
        type: "array",
        items: { type: "string" },
        description:
          "Package paths to rescan, e.g. ['/Game/Materials']. Defaults to " +
          "['/Game'] when omitted (never the whole registry incl. /Engine " +
          "by accident). A single-string `path` is deduped into this list.",
      },
      path: {
        type: "string",
        description:
          "Single package path to rescan (alternative to `paths`). Deduped " +
          "into `paths` when both are supplied.",
      },
      force: {
        type: "boolean",
        default: false,
        description:
          "Force a full blocking rescan of the requested paths. Slow for " +
          "large trees; leave false to rescan only modified files.",
      },
    },
    additionalProperties: false,
  },
};
