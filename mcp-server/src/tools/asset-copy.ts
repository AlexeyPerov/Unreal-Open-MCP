import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P4.2 — asset copy / duplicate. Wraps UEditorAssetLibrary::DuplicateAsset so
// an agent can duplicate an asset without composing editor scripts. `source`
// is the existing asset to copy (path-or-name); `destination` is the new
// package path. The destination MUST NOT already exist — a collision is a
// structured asset_already_exists error (no silent overwrite, no data loss).
//
// Mutating: runs the full gate path (checkpoint -> copy -> validate -> delta);
// `paths_hint` should list the destination path (the new asset is what the
// gate validates).
//
// Intentional deltas vs Unity's assets_copy:
//   - Single { source, destination } pair per call replaces Unity's
//     `entries[]` batch. Unreal-MCP ships single-entry tools; batching can
//     layer later if a workflow needs it.
//   - Path roots are `/Game/...`, not `Assets/`. Destination parent folder
//     must already exist (call asset_create_folder first).
//   - Writable-root guard replaces Unity's Assets/-rooted check.
//
// Route: live (POST /tools/unreal_open_mcp_asset_copy). Mutating.
export const assetCopy: Tool = {
  name: "unreal_open_mcp_asset_copy",
  description:
    "Copy (duplicate) an asset to a new package path. `source` is the existing " +
    "asset (path-or-name, e.g. '/Game/Mat/M_Foo' or '/Game/Mat/M_Foo.M_Foo'); " +
    "`destination` is the new package path (e.g. '/Game/Mat/M_Foo_Copy'). The " +
    "destination must NOT already exist — a collision is a structured " +
    "asset_already_exists error (no silent overwrite, no data loss). " +
    "Destination parent folder must already exist (call asset_create_folder " +
    "first). Refuses /Engine, /Script, /Temp with invalid_content_root. " +
    "Mutating: runs the full gate path (checkpoint -> copy -> validate -> " +
    "delta); `paths_hint` MUST list the destination path (e.g. " +
    "['/Game/Mat/M_Foo_Copy']) — there is no whole-project fallback, set " +
    "gate:\"off\" to bypass. Result shape: { source, destination }. Error " +
    "codes: missing_parameter (no source/destination), asset_not_found (source " +
    "missing), invalid_path (destination not a valid package path), " +
    "invalid_content_root (engine root), asset_already_exists (destination " +
    "collision), execution_error (DuplicateAsset returned false).",
  inputSchema: {
    type: "object",
    required: ["source", "destination", "paths_hint"],
    properties: {
      source: {
        type: "string",
        description:
          "Existing asset path-or-name to copy from — an object path " +
          "('/Game/Mat/M_Foo.M_Foo') or a package path ('/Game/Mat/M_Foo'). " +
          "Both forms resolve through the AssetRegistry.",
      },
      destination: {
        type: "string",
        description:
          "Destination package path for the duplicate, e.g. " +
          "'/Game/Mat/M_Foo_Copy'. Must NOT already exist (collision is a " +
          "structured error). Parent folder must already exist.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — destination asset path(s) the mutation is scoped " +
          "to, fed to the gate as the checkpoint + validate hint. REQUIRED " +
          "for mutating tools (the gate refuses an empty hint with " +
          "paths_hint_required; there is no whole-project fallback). Set " +
          "gate:\"off\" to bypass the gate and skip the hint.",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint -> copy -> " +
          "validate -> delta and hard-fails on new Errors; warn commits " +
          "the mutation but surfaces new Errors as warnings; off skips " +
          "the gate entirely (paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};
