import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P4.2 — Content Browser folder creation. Wraps UEditorAssetLibrary::MakeDirectory
// so an agent can organize project content without composing execute_csharp.
// Idempotent — succeeds (created: false) when the folder already exists; never
// an error. The writable-root guard refuses /Engine, /Script, /Temp so an agent
// cannot accidentally scribble into engine content.
//
// Mutating: runs the full gate path (checkpoint -> create -> validate -> delta).
// `paths_hint` must list the created folder path (the gate's validation scope);
// there is no whole-project fallback.
//
// Intentional deltas vs Unity's assets_create_folder:
//   - Single `path` per call replaces Unity's `folders[]` batch of
//     { parent_folder_path, new_folder_name }. Unreal-MCP ships single-path
//     tools; batching can layer later if a workflow needs it.
//   - Path roots are `/Game/...`, not `Assets/`. Intermediate parents must
//     already exist (call asset_create_folder in order, root first).
//   - Writable-root guard replaces Unity's Assets/-rooted check — engine
//     content (/Engine, /Script, /Temp) is refused on writes.
//
// Route: live (POST /tools/unreal_open_mcp_asset_create_folder). Mutating.
export const assetCreateFolder: Tool = {
  name: "unreal_open_mcp_asset_create_folder",
  description:
    "Create a Content Browser folder at a package path like '/Game/MyFolder'. " +
    "Idempotent — returns { path, created: false } when the folder already " +
    "exists (NOT an error). Intermediate parents must already exist (call " +
    "asset_create_folder in order, root first). Refuses /Engine, /Script, " +
    "/Temp with invalid_content_root so engine content cannot be silently " +
    "scribbled into. Mutating: runs the full gate path " +
    "(checkpoint -> create -> validate -> delta); `paths_hint` MUST list the " +
    "created folder path (e.g. ['/Game/MyFolder']) — there is no " +
    "whole-project fallback, set gate:\"off\" to bypass. Result shape: " +
    "{ path, created }. Error codes: missing_parameter (no path), " +
    "invalid_parameter (malformed body), invalid_content_root (engine root), " +
    "execution_error (MakeDirectory returned false).",
  inputSchema: {
    type: "object",
    required: ["path", "paths_hint"],
    properties: {
      path: {
        type: "string",
        description:
          "Package path of the folder to create, e.g. '/Game/MyFolder' or " +
          "'/Game/MyFolder/Sub'. Must be under a project / plugin content " +
          "root (not /Engine, /Script, /Temp). Intermediate parents must " +
          "already exist.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — content path(s) the mutation is scoped to, fed " +
          "to the gate as the checkpoint + validate hint. REQUIRED for " +
          "mutating tools (the gate refuses an empty hint with " +
          "paths_hint_required; there is no whole-project fallback). Set " +
          "gate:\"off\" to bypass the gate and skip the hint.",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint -> create -> " +
          "validate -> delta and hard-fails on new Errors; warn commits " +
          "the mutation but surfaces new Errors as warnings; off skips " +
          "the gate entirely (paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};
