import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P7.1 — enumerate project source files under <Project>/Source/. Read-only and
// gate-free. Adapts the Unreal-MCP source-list scoped-inventory behavior: an
// optional `module` folder scopes the listing to Source/<module>/; `recursive`
// (default true) walks sub-folders; an optional `extensions` array overrides
// the default allow-list (.h/.hpp/.c/.cc/.cpp/.cs). Every path access is
// JAILED to <Project>/Source — a `module` that escapes returns
// path_escapes_jail.
//
// Route: live (POST /tools/unreal_open_mcp_source_list). Read-only.
export const sourceList: Tool = {
  name: "unreal_open_mcp_source_list",
  description:
    "Enumerate project source files under <Project>/Source/. Read-only and " +
    "gate-free. Scoped to `module` (a folder under Source/) when given, else " +
    "the whole <Project>/Source tree. `recursive` (default true) walks " +
    "sub-folders. By default lists .h/.hpp/.c/.cc/.cpp/.cs; pass `extensions` " +
    "(array of extensions without the dot) to override. ALL access is JAILED " +
    "to <Project>/Source — a `module` that escapes (traversal / absolute-" +
    "outside) returns `path_escapes_jail`. Returns { root, files:[{path," +
    "bytes}], count, total_bytes } where each file path is relative to " +
    "Source/, sorted. Note: the jail targets the PROJECT Source/, not " +
    "Plugins/UnrealOpenMCP, and the default extension allow-list excludes " +
    "Intermediate/ build junk (those are not source extensions). Error codes: " +
    "invalid_parameter (malformed body), path_escapes_jail, module_not_found.",
  inputSchema: {
    type: "object",
    properties: {
      module: {
        type: "string",
        description:
          "Module folder under Source/ to scope the listing (e.g. 'MyGame'). " +
          "Omit for the whole Source/ tree.",
      },
      recursive: {
        type: "boolean",
        default: true,
        description: "Recurse sub-folders. Default true.",
      },
      extensions: {
        type: "array",
        items: { type: "string" },
        description:
          "Extensions to include (without the dot), e.g. ['h','cpp']. Default " +
          "['h','hpp','c','cc','cpp','cs'].",
      },
    },
    additionalProperties: false,
  },
};
