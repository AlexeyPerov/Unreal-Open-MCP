import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P7.1 — read a project C++ source file under <Project>/Source/ with optional
// line slicing. Read-only and gate-free. The Unreal analog of Unity's
// script-read (line slice + max_lines + project-root refusal), adapted to a
// Source/-only jail (not Assets/Packages). Every path access is JAILED:
// relative-to-Source or absolute-inside-Source only; `..`, absolute-outside,
// and NTFS alternate-data-stream (`:`) escapes are refused with a structured
// `path_escapes_jail` error — they never read.
//
// Route: live (POST /tools/unreal_open_mcp_source_read). Read-only.
export const sourceRead: Tool = {
  name: "unreal_open_mcp_source_read",
  description:
    "Read a project C++ source file (.h/.hpp/.c/.cc/.cpp) from disk under " +
    "<Project>/Source/. Read-only and gate-free. `path` is relative to the " +
    "project Source/ directory (e.g. 'MyGame/MyActor.cpp') or an absolute " +
    "path inside it; ALL access is JAILED to <Project>/Source — traversal " +
    "('../'), absolute-outside, and NTFS alternate-data-stream (':') escapes " +
    "return a structured `path_escapes_jail` error and never read. Returns " +
    "{ path, total_lines, start_line, end_line, truncated, lines:[{line," +
    "text}] } where each entry is a 1-based numbered line. Optionally window " +
    "the result by `start_line` / `end_line` (1-based, inclusive; end_line " +
    "default 0 = through end of file) and cap it with `max_lines` (default " +
    "2000, hard cap 20000) — when the requested window exceeds the cap, " +
    "`truncated` is true and only the first `max_lines` lines are returned. " +
    "Use this to inspect an existing class before a create/update/compile " +
    "loop. Note: the jail targets the PROJECT Source/, not " +
    "Plugins/UnrealOpenMCP. Error codes: missing_parameter (no path), " +
    "invalid_parameter (malformed body), path_escapes_jail, file_not_found, " +
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
