import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P4.2 — asset move / rename. Wraps UEditorAssetLibrary::RenameAsset.
// `source` is the existing asset (path-or-name); `destination` is the new
// package path. Destination must NOT already exist — a collision is a
// structured asset_already_exists error. A redirector MAY remain at the
// source path (the structured result surfaces this as a `note` field); fix
// up references separately with asset_refresh + apply_fix.
//
// Mutating: runs the full gate path (checkpoint -> move -> validate ->
// delta); `paths_hint` should include BOTH source AND destination paths so
// the gate's broken_soft_references rule can flag references that pointed
// at the old path.
//
// Intentional deltas vs Unity's assets_move:
//   - Single { source, destination } pair per call replaces Unity's
//     `entries[]` batch.
//   - Path roots are `/Game/...`, not `Assets/`.
//   - Writable-root guard replaces Unity's Assets/-rooted check.
//   - The redirector note surfaces explicitly in the structured result so
//     the agent knows references to the old path may need fixing.
//
// Route: live (POST /tools/unreal_open_mcp_asset_move). Mutating.
export const assetMove: Tool = {
  name: "unreal_open_mcp_asset_move",
  description:
    "Move or rename an asset. `source` is the existing asset (path-or-name); " +
    "`destination` is the new package path (e.g. '/Game/Mat/M_Foo_Renamed'). " +
    "The destination must NOT already exist — a collision is a structured " +
    "asset_already_exists error. Destination parent folder must already " +
    "exist. A redirector MAY remain at the source path — surfaced as a " +
    "`note` field in the result; fix up references separately (asset_refresh " +
    "+ apply_fix for broken_soft_references). Refuses /Engine, /Script, " +
    "/Temp with invalid_content_root. Mutating: runs the full gate path " +
    "(checkpoint -> move -> validate -> delta); `paths_hint` SHOULD include " +
    "BOTH source AND destination paths so the gate's broken_soft_references " +
    "rule can flag references that pointed at the old path — there is no " +
    "whole-project fallback, set gate:\"off\" to bypass. Result shape: " +
    "{ source, destination, note }. Error codes: missing_parameter (no " +
    "source/destination), asset_not_found (source missing), invalid_path " +
    "(destination not a valid package path), invalid_content_root (engine " +
    "root), asset_already_exists (destination collision), execution_error " +
    "(RenameAsset returned false).",
  inputSchema: {
    type: "object",
    required: ["source", "destination", "paths_hint"],
    properties: {
      source: {
        type: "string",
        description:
          "Existing asset path-or-name to move from — an object path " +
          "('/Game/Mat/M_Foo.M_Foo') or a package path ('/Game/Mat/M_Foo').",
      },
      destination: {
        type: "string",
        description:
          "Destination package path (e.g. '/Game/Mat/M_Foo_Renamed'). Must " +
          "NOT already exist (collision is a structured error). Parent " +
          "folder must already exist.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — include BOTH source and destination paths so " +
          "the gate can detect dangling references that pointed at the old " +
          "path. REQUIRED for mutating tools (the gate refuses an empty hint " +
          "with paths_hint_required; there is no whole-project fallback). " +
          "Set gate:\"off\" to bypass the gate and skip the hint.",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint -> move -> " +
          "validate -> delta and hard-fails on new Errors; warn commits " +
          "the mutation but surfaces new Errors as warnings; off skips " +
          "the gate entirely (paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};
