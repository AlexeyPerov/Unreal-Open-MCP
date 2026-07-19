import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P4.2 — asset delete. Wraps UEditorAssetLibrary::DeleteAsset with a
// referencer guard: by default the call REFUSES with a structured
// `delete_blocked_by_referencers` error listing the inbound referencer
// packages so the agent can decide. Pass `force: true` to delete anyway
// (the gate's broken_soft_references rule will still flag any dangling
// refs in the validate pass).
//
// This is the P4.2 contract: "Delete surfaces referencer information (or
// structured refuse) before breaking soft refs silently." Without `force`,
// the AssetRegistry is queried for inbound referencers and the bounded
// list (capped at 50 in the structured payload, 10 in the message) is
// surfaced alongside the structured refuse.
//
// Mutating: runs the full gate path (checkpoint -> delete -> validate ->
// delta); `paths_hint` should list the deleted asset path.
//
// Intentional deltas vs Unity's assets_delete:
//   - Single `path` per call replaces Unity's `paths[]` batch.
//   - Path roots are `/Game/...`, not `Assets/`.
//   - Writable-root guard refuses /Engine, /Script, /Temp (delete is the
//     most destructive write — force:true on engine content would corrupt
//     the install).
//   - Referencer guard uses Unreal AssetRegistry::GetReferencers instead of
//     Unity's missing-script surface.
//
// Route: live (POST /tools/unreal_open_mcp_asset_delete). Mutating.
export const assetDelete: Tool = {
  name: "unreal_open_mcp_asset_delete",
  description:
    "Delete an asset by path-or-name. Destructive and NOT undoable from MCP. " +
    "By default REFUSES with `delete_blocked_by_referencers` when other " +
    "on-disk packages reference the asset — the error message embeds the " +
    "referencer count + a bounded referencer list (capped at 10, with a " +
    "'(+N more)' tail) so you can decide. Pass `force: true` to delete " +
    "anyway (the gate's broken_soft_references rule will still flag any " +
    "dangling refs in the validate pass). Refuses /Engine, /Script, /Temp " +
    "with invalid_content_root. Mutating: runs the full gate path " +
    "(checkpoint -> delete -> validate -> delta); `paths_hint` MUST list " +
    "the deleted asset path so the gate can flag dangling references that " +
    "pointed at it — there is no whole-project fallback, set gate:\"off\" " +
    "to bypass. Result shape: { path, deleted, forced }. Error codes: " +
    "missing_parameter (no path), asset_not_found (path missing), " +
    "invalid_content_root (engine root), delete_blocked_by_referencers " +
    "(asset referenced; pass force:true), execution_error (DeleteAsset " +
    "returned false).",
  inputSchema: {
    type: "object",
    required: ["path", "paths_hint"],
    properties: {
      path: {
        type: "string",
        description:
          "Asset path-or-name to delete — an object path " +
          "('/Game/Mat/M_Foo.M_Foo') or a package path ('/Game/Mat/M_Foo'). " +
          "Must be under a project / plugin content root (not /Engine, " +
          "/Script, /Temp).",
      },
      force: {
        type: "boolean",
        default: false,
        description:
          "Delete even when the asset is referenced by other packages. " +
          "Default false. When true, the gate's broken_soft_references " +
          "rule will flag any dangling refs in the validate pass.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — deleted asset path(s) so the gate can flag " +
          "dangling references that pointed at it. REQUIRED for mutating " +
          "tools (the gate refuses an empty hint with paths_hint_required; " +
          "there is no whole-project fallback). Set gate:\"off\" to bypass " +
          "the gate and skip the hint.",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint -> delete -> " +
          "validate -> delta and hard-fails on new Errors; warn commits " +
          "the mutation but surfaces new Errors as warnings; off skips " +
          "the gate entirely (paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};
