import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P4.1 — read-only asset metadata drill-down. Returns AssetRegistry metadata
// for a single asset: { name, path, package, class, tags }. The Unreal
// analog of Unity's read_asset, adapted to Unreal's live AssetRegistry model
// (ADR-006: live-only, no offline .uasset parse).
//
// `path` accepts the path-or-name convention the asset family documents — an
// object path ('/Game/Mat/M_Foo.M_Foo') or a package path
// ('/Game/Mat/M_Foo'); both forms resolve through DoesAssetExist /
// FindAssetData.
//
// Optional `paths` (dot-separated field names) projects the result to those
// branches only — saves tokens when the caller needs just one field (e.g.
// 'class' to branch on type). An absent branch is dropped silently; a
// non-array `paths` is an invalid_parameter error.
//
// Intentional deltas vs Unity's read_asset:
//   - Live AssetRegistry, not offline Assets/ parse (ADR-006).
//   - Returns registry metadata + tags, not a full property / hierarchy dump.
//     Property drill-down stays on actor/object tools / later work.
//   - Path roots are `/Game/...`, not `Assets/`.
//
// Route: live (POST /tools/unreal_open_mcp_asset_get_data). Read-only — no
// gate.
export const assetGetData: Tool = {
  name: "unreal_open_mcp_asset_get_data",
  description:
    "Read AssetRegistry metadata for one asset by path-or-name. Read-only " +
    "(gate-free). Returns { name, path, package, class, tags } where `tags` " +
    "is the asset's registry tag map (key → string value; empty when the " +
    "asset has no tags). `path` accepts either an object path " +
    "('/Game/Mat/M_Foo.M_Foo') or a package path ('/Game/Mat/M_Foo') — both " +
    "resolve through the AssetRegistry. Optional `paths` (dot-separated " +
    "field names like ['name','class']) projects the result to those " +
    "branches only — saves tokens when you need one field. Use after " +
    "asset_find to drill into a specific asset; chain into actor_modify / " +
    "object_modify for property writes (later phases). Error codes: " +
    "missing_parameter (path absent), asset_not_found (no asset at path), " +
    "invalid_parameter (malformed body or non-array `paths`).",
  inputSchema: {
    type: "object",
    required: ["path"],
    properties: {
      path: {
        type: "string",
        description:
          "Asset path-or-name — an object path " +
          "('/Game/Mat/M_Foo.M_Foo') or a package path ('/Game/Mat/M_Foo'). " +
          "Both forms resolve through the AssetRegistry.",
      },
      paths: {
        type: "array",
        items: { type: "string" },
        description:
          "Optional scoped projection — dot-separated field names " +
          "(['name','class']) to emit only those branches of the result and " +
          "save tokens. An absent branch is dropped silently. Must be an " +
          "array; a non-array value is rejected with invalid_parameter.",
      },
    },
    additionalProperties: false,
  },
};
