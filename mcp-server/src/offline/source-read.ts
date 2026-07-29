// Offline source-read jail + reader for `unreal_open_mcp_source_read_offline`.
//
// Mirrors the P7.1 `<Project>/Source/` jail the bridge's `ResolveJailedPath`
// enforces (packages/bridge/.../UnrealOpenMcpSourceTools.h), ported to
// TypeScript so the offline reader rejects the SAME escapes the live tool
// does. Every path access is JAILED to `<Project>/Source/`:
//   - relative-to-Source or absolute-inside-Source → resolved + read
//   - `..` traversal, absolute-outside, and NTFS alternate-data-stream (`:`)
//     escapes → `path_escapes_jail` (never read)
//
// The jail targets the PROJECT `Source/`, not `Plugins/UnrealOpenMCP`.
//
// Adapted from Unity's offline script-read spirit (Unity's offline reader is
// asset-focused; this is the Unreal Source/ analog). No new deps beyond node
// built-ins.

import { existsSync, readFileSync, statSync } from "node:fs";
import { join, resolve, relative, isAbsolute, sep } from "node:path";

/** Hard cap on file size we are willing to load into memory. Mirrors the
 *  bridge's MaxReadableBytes (64 MiB) — the max_lines cap bounds the RETURNED
 *  slice, not the read, so this guard refuses an absurdly large file first. */
const MAX_READABLE_BYTES = 64 * 1024 * 1024;

/** Soft cap on returned lines (default + hard ceiling for a caller-supplied
 *  max_lines). Mirrors the bridge's DefaultMaxLines / HardMaxLines. */
const DEFAULT_MAX_LINES = 2000;
const HARD_MAX_LINES = 20000;

/** Result of a jailed path resolution. */
export type ResolveJailedResult =
  | { ok: true; absolute: string; relative: string }
  | { ok: false; code: "path_escapes_jail" };

/**
 * Resolve a caller-supplied `path` against the project `Source/` root with the
 * same jail rules the bridge's `ResolveJailedPath` enforces:
 *   - relative path (no leading `/` or drive) → resolved under Source/
 *   - absolute path inside Source/ → accepted
 *   - `..` that escapes Source/, absolute-outside, and NTFS ADS (`:`) → refused
 *
 * The NTFS alternate-data-stream (`:`) check rejects Windows `file.cpp:stream`
 * escapes and Unix colon-bearing names that are never valid C++ source paths.
 * Backslashes are normalized to forward slashes before the jail check so a
 * Windows-style `..\..` is caught the same as `../..`.
 *
 * Returns the absolute resolved path + the path relative to Source/ on success,
 * or `{ ok:false, code:"path_escapes_jail" }` on escape. Never throws.
 */
export function resolveJailedSourcePath(
  projectPath: string,
  inputPath: string,
): ResolveJailedResult {
  if (!inputPath || typeof inputPath !== "string") {
    return { ok: false, code: "path_escapes_jail" };
  }
  // NTFS alternate-data-stream / colon escape — refuse outright, EXCEPT a
  // leading Windows drive-letter prefix (`C:`) which is a valid absolute path
  // (the absolute-outside check below handles whether it is inside Source/). A
  // colon anywhere else (`file.cpp:stream`, `http://...`) is never a valid C++
  // source path and is treated as a jail escape.
  const driveStripped = inputPath.replace(/^[A-Za-z]:/, "");
  if (driveStripped.includes(":")) {
    return { ok: false, code: "path_escapes_jail" };
  }
  const sourceRoot = resolve(projectPath, "Source");
  // Normalize backslashes → forward slashes for the traversal check so a
  // Windows-style escape (`..\..`) is caught identically.
  const normalized = inputPath.replace(/\\/g, "/");
  // Resolve against the Source root whether the input is relative or absolute.
  // For an absolute-inside-Source path, join collapses it onto the root; for an
  // absolute-outside path, resolve() yields a path outside Source/ and the
  // containment check below catches it.
  const candidate = isAbsolute(normalized)
    ? resolve(normalized)
    : resolve(sourceRoot, normalized);
  const rel = relative(sourceRoot, candidate);
  // `relative()` returns a path starting with `..` when the candidate is outside
  // the root; an empty string means the root itself (a directory, refused later
  // by the file read). A leading `..` segment OR an absolute (drive-letter on
  // Windows) relative result is an escape.
  if (rel.startsWith("..") || isAbsolute(rel)) {
    return { ok: false, code: "path_escapes_jail" };
  }
  return { ok: true, absolute: candidate, relative: rel };
}

export interface SourceReadResult {
  /** Path relative to Source/. */
  path: string;
  /** Total line count of the file. */
  total_lines: number;
  /** First line returned (1-based, inclusive). */
  start_line: number;
  /** Last line returned (1-based, inclusive; 0 = through end of file). */
  end_line: number;
  /** Whether the requested window exceeded the max_lines cap. */
  truncated: boolean;
  /** Numbered lines (1-based `line` + `text`). */
  lines: Array<{ line: number; text: string }>;
}

export type SourceReadOutcome =
  | { ok: true; result: SourceReadResult }
  | { ok: false; code: "path_escapes_jail" | "file_not_found" | "not_a_file" | "read_failed" };

/**
 * Read a project C++ source file under `<Project>/Source/` with an optional
 * 1-based line window + a soft max_lines cap. Mirrors the live
 * `source_read` contract (`{ path, total_lines, start_line, end_line,
 * truncated, lines:[{line,text}] }`) so an agent treats an offline and a live
 * read identically.
 *
 * Args mirror the live tool: `start_line` (1-based, inclusive, default 1),
 * `end_line` (1-based, inclusive, default 0 = through EOF), `max_lines`
 * (default 2000, hard ceiling 20000).
 */
export function readSourceOffline(
  projectPath: string,
  inputPath: string,
  opts: {
    start_line?: number;
    end_line?: number;
    max_lines?: number;
  } = {},
): SourceReadOutcome {
  const resolved = resolveJailedSourcePath(projectPath, inputPath);
  if (!resolved.ok) return { ok: false, code: resolved.code };

  if (!existsSync(resolved.absolute)) {
    return { ok: false, code: "file_not_found" };
  }
  let stat;
  try {
    stat = statSync(resolved.absolute);
  } catch {
    return { ok: false, code: "read_failed" };
  }
  if (!stat.isFile()) {
    return { ok: false, code: "not_a_file" };
  }
  if (stat.size > MAX_READABLE_BYTES) {
    return { ok: false, code: "read_failed" };
  }

  let content: string;
  try {
    content = readFileSync(resolved.absolute, "utf8");
  } catch {
    return { ok: false, code: "read_failed" };
  }

  // ParseIntoArrayLines-equivalent: split on \n, drop a trailing empty produced
  // by a newline-terminated file (mirrors the bridge's line accounting).
  const allLines = content.split(/\r?\n/);
  if (allLines.length > 0 && allLines[allLines.length - 1] === "") {
    allLines.pop();
  }
  const totalLines = allLines.length;

  const startLine = Math.max(1, Math.floor(opts.start_line ?? 1));
  const requestedEnd = opts.end_line && opts.end_line > 0 ? opts.end_line : totalLines;
  const maxLines = Math.min(
    HARD_MAX_LINES,
    Math.max(1, Math.floor(opts.max_lines ?? DEFAULT_MAX_LINES)),
  );

  // Clamp the 1-based inclusive window to the file. start beyond EOF → empty.
  const clampedStart = Math.min(startLine, totalLines + 1);
  const clampedEnd = Math.min(requestedEnd, totalLines);
  const windowSize = Math.max(0, clampedEnd - clampedStart + 1);
  const truncated = windowSize > maxLines;
  const effectiveEnd = truncated ? clampedStart + maxLines - 1 : clampedEnd;

  const lines: Array<{ line: number; text: string }> = [];
  for (let i = clampedStart; i <= effectiveEnd; i++) {
    lines.push({ line: i, text: allLines[i - 1] ?? "" });
  }

  return {
    ok: true,
    result: {
      path: resolved.relative,
      total_lines: totalLines,
      start_line: clampedStart,
      end_line: effectiveEnd,
      truncated,
      lines,
    },
  };
}
