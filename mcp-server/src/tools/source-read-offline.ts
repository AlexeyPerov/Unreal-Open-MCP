import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P8.7 — offline source_read_offline: read a project C++ source file under
// <Project>/Source/ from disk, with the bridge DOWN. The offline twin of the
// live `source_read` (P7.1): same jail (relative-to-Source or absolute-inside;
// reject `..`, absolute-outside, and NTFS ADS `:` escapes), same result shape
// (`{ path, total_lines, start_line, end_line, truncated, lines:[{line,text}] }`),
// same `start_line`/`end_line`/`max_lines` windowing — ported to TypeScript so
// the offline reader rejects the SAME escapes the live tool does.
//
// A dedicated offline tool (rather than an offline-fallback on the live
// `source_read`) keeps the router policy table clean: the live tool stays a
// pure bridge POST, the offline tool stays a pure disk read. An agent picks the
// offline tool when the bridge is down and it needs to inspect source the
// live tool cannot reach.
//
// Route: **offline** (always — resolved from disk; never hits the bridge). The
// router stamps `_source: "offline"` + `_route: { route: "offline" }`.
//
// SCOPE (ADR-006): project source text only — no `.uasset` offline parse. The
// jail targets the PROJECT `Source/`, not `Plugins/UnrealOpenMCP`.
export const sourceReadOffline: Tool = {
  name: "unreal_open_mcp_source_read_offline",
  description:
    "Read a project C++ source file (.h/.hpp/.c/.cc/.cpp) from disk under " +
    "<Project>/Source/, with the bridge DOWN. The offline twin of " +
    "unreal_open_mcp_source_read: same Source/ jail, same result shape " +
    "({ path, total_lines, start_line, end_line, truncated, lines:[{line," +
    "text}] }), same start_line/end_line/max_lines windowing. Use this when " +
    "the bridge is unreachable and you need to inspect project C++ the live " +
    "source_read cannot reach. `path` is relative to the project Source/ " +
    "directory (e.g. 'MyGame/MyActor.cpp') or an absolute path inside it; ALL " +
    "access is JAILED to <Project>/Source — traversal ('../'), absolute-" +
    "outside, and NTFS alternate-data-stream (':') escapes return a structured " +
    "`path_escapes_jail` error and never read. Optionally window the result by " +
    "start_line / end_line (1-based, inclusive; end_line default 0 = through " +
    "end of file) and cap it with max_lines (default 2000, hard cap 20000) — " +
    "when the requested window exceeds the cap, `truncated` is true and only " +
    "the first max_lines lines are returned. Note: the jail targets the PROJECT " +
    "Source/, not Plugins/UnrealOpenMCP. SCOPE (ADR-006): source text only — no " +
    ".uasset offline parse. Route: offline (always). Error codes: " +
    "missing_parameter (no path), path_escapes_jail, file_not_found, " +
    "not_a_file (path is a directory), read_failed.",
  inputSchema: {
    type: "object",
    required: ["path"],
    properties: {
      path: {
        type: "string",
        description:
          "Source file path relative to <Project>/Source (or absolute, inside " +
          "it), e.g. 'MyGame/MyActor.cpp'. JAILED — escapes return " +
          "path_escapes_jail.",
      },
      start_line: {
        type: "integer",
        default: 1,
        minimum: 1,
        description: "First line to return (1-based, inclusive).",
      },
      end_line: {
        type: "integer",
        default: 0,
        minimum: 0,
        description: "Last line to return (1-based, inclusive). 0 = through end of file.",
      },
      max_lines: {
        type: "integer",
        default: 2000,
        minimum: 1,
        description:
          "Hard cap on the number of lines returned (default 2000, ceiling " +
          "20000). Larger windows are truncated and reported via `truncated`.",
      },
    },
    additionalProperties: false,
  },
};
