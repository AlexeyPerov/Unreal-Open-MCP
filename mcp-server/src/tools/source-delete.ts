import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P7.2 — delete a single source file under <Project>/Source/. Refuses
// directories (is_directory) and missing paths (file_not_found). Destructive
// and NOT undoable from MCP — there is no recycle bin, no FScopedTransaction
// (source files are not UObject assets), so the gate checkpoint is the only
// safety net. Pass gate:"off" only when you accept that.
//
// Every path access is JAILED to <Project>/Source — `..`, absolute-outside, and
// NTFS ADS (`:`) escapes return `path_escapes_jail` and never delete.
//
// Mutating (destructive): runs the full gate path (checkpoint -> delete ->
// validate -> delta); `paths_hint` MUST list the file path relative to Source/
// (e.g. ['MyGame/MyActor.cpp']) — there is no whole-project fallback, set
// gate:"off" to bypass.
//
// Route: live (POST /tools/unreal_open_mcp_source_delete). Mutating.
export const sourceDelete: Tool = {
  name: "unreal_open_mcp_source_delete",
  description:
    "Delete a single project source file under <Project>/Source/. Refuses " +
    "directories (is_directory) and missing paths (file_not_found). DESTRUCTIVE " +
    "and NOT undoable from MCP — source files are not UObject assets, so there " +
    "is no FScopedTransaction / recycle bin; the gate checkpoint is the only " +
    "safety net. JAILED to <Project>/Source — traversal / absolute-outside / " +
    "NTFS ADS (':') escapes return path_escapes_jail and never delete. Mutating " +
    "(destructive): runs the full gate path (checkpoint -> delete -> validate " +
    "-> delta); `paths_hint` MUST list the file path relative to Source/ (e.g. " +
    "['MyGame/MyActor.cpp']) — there is no whole-project fallback, set " +
    "gate:\"off\" to bypass. Result shape: { path, deleted:true }. Error codes: " +
    "missing_parameter (no path) / invalid_parameter (malformed body) / " +
    "file_not_found / is_directory / path_escapes_jail / delete_failed.",
  inputSchema: {
    type: "object",
    required: ["path", "paths_hint"],
    properties: {
      path: {
        type: "string",
        description:
          "Source file path relative to <Project>/Source (or absolute, inside " +
          "it), e.g. 'MyGame/MyActor.cpp'. Must be a FILE, not a directory. " +
          "JAILED — escapes return path_escapes_jail.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — the Source-relative file path(s) the delete is " +
          "scoped to, fed to the gate as the checkpoint + validate hint. e.g. " +
          "['MyGame/MyActor.cpp']. REQUIRED for mutating tools (the gate " +
          "refuses an empty hint with paths_hint_required; there is no whole-" +
          "project fallback). Set gate:\"off\" to bypass the gate and skip the " +
          "hint.",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint -> delete -> " +
          "validate -> delta and hard-fails on new Errors; warn commits the " +
          "delete but surfaces new Errors as warnings; off skips the gate " +
          "entirely (paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};
