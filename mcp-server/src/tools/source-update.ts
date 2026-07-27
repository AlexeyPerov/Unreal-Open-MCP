import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P7.2 — update an existing project source file: full-file replace OR a 1-based
// inclusive line-range splice. The file MUST already exist (use
// source_create_class for new files). Omit `start_line`/`end_line` to replace
// the whole file; pass BOTH to splice `content` over that inclusive line range.
// A half-range (only one of start_line/end_line) is rejected with
// invalid_parameter.
//
// When splicing, the file's dominant line ending (CRLF or LF) AND any trailing
// newline are preserved, so a one-line splice on a CRLF file is a one-line diff
// rather than a whole-file CRLF->LF rewrite that drops the final EOL. The
// phantom-trailing-empty-line artifact of ParseIntoArrayLines is dropped on BOTH
// the existing file AND the replacement content, so a newline-terminated
// `content` (the common AI-caller form) does not splice a spurious blank line.
// The range is validated in int64 BEFORE narrowing to int32 so a huge value
// cannot wrap and silently splice the wrong lines.
//
// Every path access is JAILED to <Project>/Source — `..`, absolute-outside, and
// NTFS ADS (`:`) escapes return `path_escapes_jail` and never write. Writes are
// BOM-less UTF-8 (the project source convention); a splice on a UTF-8-BOM file
// drops the BOM.
//
// Mutating: runs the full gate path (checkpoint -> update -> validate -> delta);
// `paths_hint` MUST list the file path relative to Source/ (e.g.
// ['MyGame/MyActor.cpp']) — there is no whole-project fallback, set
// gate:"off" to bypass.
//
// Intentional delta vs Unity's script_write: Unity's script family is
// whole-file only; this tool adds the line-range splice (adapted from the
// Unreal-MCP behavior reference) for surgical edits.
//
// Route: live (POST /tools/unreal_open_mcp_source_update). Mutating.
export const sourceUpdate: Tool = {
  name: "unreal_open_mcp_source_update",
  description:
    "Replace an existing source file's full contents, OR splice a 1-based " +
    "inclusive line range. The file MUST already exist (use " +
    "source_create_class to scaffold new files). Pass `content` (the " +
    "replacement text). Omit `start_line`/`end_line` to replace the whole " +
    "file; pass BOTH to splice `content` over that inclusive line range (a " +
    "half-range — only one of start_line/end_line — is rejected with " +
    "invalid_parameter). When splicing, the file's dominant line ending " +
    "(CRLF/LF) and trailing newline are PRESERVED so a one-line splice is a " +
    "one-line diff, not a whole-file EOL rewrite. The range is validated before " +
    "writing (out-of-range -> invalid_line_range, never a silent mis-edit). " +
    "JAILED to <Project>/Source — traversal / absolute-outside / NTFS ADS " +
    "escapes return path_escapes_jail and never write. Writes are BOM-less UTF-8 " +
    "(a splice on a UTF-8-BOM file drops the BOM). Mutating: runs the full gate " +
    "path (checkpoint -> update -> validate -> delta); `paths_hint` MUST list " +
    "the file path relative to Source/ (e.g. ['MyGame/MyActor.cpp']) — there is " +
    "no whole-project fallback, set gate:\"off\" to bypass. Result shape: " +
    "{ path, mode ('full'|'range'), bytes_written, lines_written }. Error " +
    "codes: missing_parameter (no path / no content) / invalid_parameter " +
    "(malformed body / half-range / non-string content) / file_not_found / " +
    "invalid_line_range / path_escapes_jail / write_failed.",
  inputSchema: {
    type: "object",
    required: ["path", "content", "paths_hint"],
    properties: {
      path: {
        type: "string",
        description:
          "Source file path relative to <Project>/Source (or absolute, inside " +
          "it), e.g. 'MyGame/MyActor.cpp'. The file MUST already exist. JAILED " +
          "— escapes return path_escapes_jail.",
      },
      content: {
        type: "string",
        description:
          "Replacement text. Full-file replace when start_line/end_line are " +
          "omitted; spliced over the [start_line..end_line] inclusive range " +
          "when both are present. An empty string is a legitimate full-file " +
          "clear (not a splice-clear — that needs an in-range splice).",
      },
      start_line: {
        type: "integer",
        minimum: 1,
        description:
          "1-based first line to replace (inclusive). REQUIRES end_line. Omit " +
          "for a full-file replace.",
      },
      end_line: {
        type: "integer",
        minimum: 1,
        description:
          "1-based last line to replace (inclusive). REQUIRES start_line. Omit " +
          "for a full-file replace.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — the Source-relative file path(s) the update is " +
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
          "Gate mode — enforce (default) runs checkpoint -> update -> " +
          "validate -> delta and hard-fails on new Errors; warn commits the " +
          "write but surfaces new Errors as warnings; off skips the gate " +
          "entirely (paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};
