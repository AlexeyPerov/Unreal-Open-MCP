import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P4.1 — read-only AssetRegistry find. Filtered query with stable ordering +
// offset/limit pagination. The Unreal analog of Unity's search_assets,
// adapted to Unreal's live AssetRegistry model (ADR-006: live-only, no
// offline .uasset parse).
//
// Filters narrow the query four ways:
//   - `name`        — case-insensitive substring on the asset name
//                     (post-filter; the registry has no substring primitive)
//   - `class_path`  — full top-level class path, e.g. '/Script/Engine.Material'
//                     (subclasses included; a short dotless name is rejected
//                     with invalid_class_path to avoid the engine ensure)
//   - `path`        — package-path scope, e.g. '/Game/Materials'
//   - `tag_key`/`tag_value` — registry tag filter (tag_value requires tag_key)
//
// When no path/class/tag is supplied the scope defaults to /Game (recursive)
// so a no-arg find never materializes the whole registry incl. /Engine.
//
// Intentional deltas vs Unity's search_assets:
//   - Live AssetRegistry, not offline Assets/ scan (ADR-006).
//   - Naming follows the Unreal-MCP style (`asset_find`), not `search_assets`.
//   - Path roots are `/Game/...`, not `Assets/`.
//   - Pagination is offset/limit (Unreal-MCP); Unity's profile/cursor axis
//     can layer later if payloads demand it.
//
// Route: live (POST /tools/unreal_open_mcp_asset_find). Read-only — no gate.
export const assetFind: Tool = {
  name: "unreal_open_mcp_asset_find",
  description:
    "Query the Content Browser AssetRegistry by name / class / path / tags. " +
    "Read-only (gate-free). Returns bounded, stably ordered results with " +
    "{ total, offset, count, assets: AssetSummary[] } so you can page " +
    " deterministically. Each AssetSummary carries { name, path, package, " +
    "class } for chaining into asset_get_data or the later P4 mutators. " +
    "Filters: `name` (case-insensitive substring on asset name), " +
    "`class_path` ('/Script/Engine.Material', subclasses included — a short " +
    "dotless name is rejected with invalid_class_path), `path` (package " +
    "scope like '/Game/Materials'), `tag_key`/`tag_value` (registry tag " +
    "match; tag_value requires tag_key), `recursive` (default true). When " +
    "no path/class/tag is supplied the scope defaults to /Game (recursive) " +
    "— a no-arg find never scans /Engine. Page with offset/limit (limit " +
    "default 100, hard max 1000). Error codes: invalid_parameter (malformed " +
    "body), missing_parameter (tag_value without tag_key), invalid_class_path " +
    "(class_path not '/Script/Module.Class' form).",
  inputSchema: {
    type: "object",
    properties: {
      name: {
        type: "string",
        description:
          "Case-insensitive substring filter on the asset name " +
          "(post-filter; the registry has no substring primitive).",
      },
      class_path: {
        type: "string",
        description:
          "Full top-level class path, e.g. '/Script/Engine.Material'. " +
          "Subclasses are included. A short dotless name ('Material') is " +
          "rejected with invalid_class_path — pass the full " +
          "'/Script/Module.Class' form.",
      },
      path: {
        type: "string",
        description:
          "Package-path scope, e.g. '/Game/Materials'. Combined with " +
          "`recursive` (default true) to control subtree depth.",
      },
      tag_key: {
        type: "string",
        description:
          "Asset-registry tag name to filter on. Use with `tag_value` to " +
          "constrain the value; omit `tag_value` to match any value.",
      },
      tag_value: {
        type: "string",
        description:
          "Required value for `tag_key`. Requires `tag_key` (a bare " +
          "tag_value is rejected with missing_parameter).",
      },
      recursive: {
        type: "boolean",
        default: true,
        description:
          "Recurse sub-paths of `path`. Default true. Applied to the default " +
          "/Game scope when no path/class/tag is supplied.",
      },
      offset: {
        type: "integer",
        minimum: 0,
        default: 0,
        description: "Pagination offset into the (stably sorted) result set.",
      },
      limit: {
        type: "integer",
        minimum: 1,
        maximum: 1000,
        default: 100,
        description:
          "Maximum results to return (1-1000, default 100). The total match " +
          "count is reported in `total` so you know how many pages remain.",
      },
    },
    additionalProperties: false,
  },
};
