// Unreal Editor.log path resolution + tail, for the offline
// `unreal_open_mcp_read_compile_errors` tool.
//
// When the bridge module itself fails to compile (Live Coding failure / a bad
// C++ edit), every in-bridge channel (console_get_logs, source_compile, the
// ping probe) is dead with it — the bridge assembly cannot load, so it cannot
// serve any handler. The ONE channel that survives is the live Editor's own
// log file under `<Project>/Saved/Logs/`: Unreal writes UBT / Live Coding /
// MSVC / clang diagnostics there regardless of bridge health. This module
// resolves that path and reads a bounded tail.
//
// Adapted from Unity Open MCP's mcp-server/src/unity-log.ts (copy fidelity for
// the tail-reader shape; intentional deltas in the path resolver — Unity reads
// a global per-user Editor.log with a project-relative 6000.5+ override, while
// Unreal writes per-project logs under `<Project>/Saved/Logs/` and rotates
// them as `<Name>.log` + `<Name>-<YYYY.MM.DD-HH.MM.SS>.log` backups).
//
// No runtime deps beyond node built-ins (mcp-server/AGENTS.md — no new deps).

import {
  existsSync,
  openSync,
  readSync,
  fstatSync,
  closeSync,
  readdirSync,
  statSync,
} from "node:fs";
import { join } from "node:path";

/**
 * The directory Unreal writes the project's editor logs into:
 * `<project>/Saved/Logs/`. Unreal creates this on first editor run; it may not
 * exist for a fresh project that has never been opened.
 */
export function editorLogsDir(projectPath: string): string {
  return join(projectPath, "Saved", "Logs");
}

/**
 * Default tail size. Bounded so a multi-MB log can't blow up the tool response;
 * 256KB is ample for a compile-error burst (Unreal writes the UBT / compiler
 * diagnostics in a contiguous block near the end of the log). Mirrors Unity's
 * DEFAULT_LOG_TAIL_BYTES.
 */
export const DEFAULT_LOG_TAIL_BYTES = 256 * 1024;

/** Upper bound on tail_bytes (1 MiB). Keeps a caller from reading the whole log. */
export const MAX_LOG_TAIL_BYTES = 1024 * 1024;

/** Lower bound on tail_bytes. A tail smaller than one diagnostic line is useless. */
export const MIN_LOG_TAIL_BYTES = 4096;

/**
 * Resolve the log files present under `<project>/Saved/Logs/`, newest-first by
 * mtime. Unreal rotates the active log (`<ProjectName>.log`) into timestamped
 * backups (`<ProjectName>-<YYYY.MM.DD-HH.MM.SS>.log`); the newest by mtime is
 * the live one the editor is currently writing. Returns an empty array when
 * the directory is absent (fresh project / never opened) or unreadable.
 *
 * Exported so tests can assert the discovery order without planting a file.
 */
export function discoverLogFiles(projectPath: string): string[] {
  const dir = editorLogsDir(projectPath);
  let names: string[];
  try {
    names = readdirSync(dir);
  } catch {
    // Missing dir or permission error — treat as "no logs". Never throw.
    return [];
  }
  const logs: Array<{ path: string; mtimeMs: number }> = [];
  for (const name of names) {
    // Only consider .log files; the directory may hold unrelated artifacts.
    if (!name.endsWith(".log")) continue;
    const full = join(dir, name);
    try {
      const st = statSync(full);
      if (st.isFile()) logs.push({ path: full, mtimeMs: st.mtimeMs });
    } catch {
      // A vanished-while-listing entry is skipped, not fatal.
    }
  }
  // Newest first. Ties broken by path for deterministic test output.
  logs.sort((a, b) => b.mtimeMs - a.mtimeMs || a.path.localeCompare(b.path));
  return logs.map((l) => l.path);
}

/**
 * Pick the authoritative log to read for compile-error extraction. Returns the
 * newest `.log` under `<project>/Saved/Logs/` by mtime, or null when the
 * directory has no logs (fresh project, never opened, or a custom `-logFile`
 * path this resolver does not cover).
 *
 * Resolution order (pinned in tests):
 *   1. `<project>/Saved/Logs/*.log` — newest by mtime (the live log the editor
 *      is currently writing, or the most recent rotated backup).
 *
 * There is intentionally NO global per-user fallback (Unity has one; Unreal
 * writes per-project logs only). When no project log exists, the tool reports
 * `log_not_found` honestly rather than reading an unrelated file.
 */
export function resolveEditorLogPath(projectPath: string): string | null {
  const logs = discoverLogFiles(projectPath);
  return logs.length > 0 ? logs[0] : null;
}

export interface ReadLogTailResult {
  /** Absolute path that was read. */
  path: string;
  /** Whether the file existed and was read. */
  exists: boolean;
  /** The tail content. Empty when the file is missing or unreadable. */
  content: string;
  /** Bytes read (content length in UTF-8 bytes). 0 when missing. */
  bytes: number;
  /** Error message when the file existed but could not be read. */
  error?: string;
}

/**
 * Read up to `maxBytes` from the END of a file, as a UTF-8 string. Returns
 * `{ exists: false }` when the file is absent. Never throws — read failures
 * (permissions, vanished mid-read) surface as `{ exists, error }`.
 *
 * The tail is read by seeking to (size - maxBytes) and reading forward, so a
 * multi-MB log is not loaded in full. Clamps `maxBytes` to
 * [MIN_LOG_TAIL_BYTES, MAX_LOG_TAIL_BYTES].
 *
 * Adapted from Unity's readLogTail (copy fidelity for the loop + error shape).
 */
export function readLogTail(
  path: string,
  maxBytes: number = DEFAULT_LOG_TAIL_BYTES,
): ReadLogTailResult {
  const capped = Math.min(
    MAX_LOG_TAIL_BYTES,
    Math.max(MIN_LOG_TAIL_BYTES, Math.floor(maxBytes)),
  );
  if (!existsSync(path)) {
    return { path, exists: false, content: "", bytes: 0 };
  }
  let fd: number | undefined;
  try {
    fd = openSync(path, "r");
    const stat = fstatSync(fd);
    const size = stat.size;
    const readLen = Math.min(size, Math.max(0, capped));
    const start = size - readLen;
    const buf = Buffer.alloc(readLen);
    // readSync may return fewer bytes than requested if the file is being
    // written concurrently; loop until the buffer is filled or we hit EOF.
    let read = 0;
    while (read < readLen) {
      // openSync's positional read overload (offset) is used so we don't rely
      // on the file pointer's current position.
      const n = readSync(fd, buf, read, readLen - read, start + read);
      if (n === 0) break;
      read += n;
    }
    return {
      path,
      exists: true,
      content: buf.subarray(0, read).toString("utf8"),
      bytes: read,
    };
  } catch (err) {
    return {
      path,
      exists: true,
      content: "",
      bytes: 0,
      error: err instanceof Error ? err.message : String(err),
    };
  } finally {
    if (fd !== undefined) {
      try {
        closeSync(fd);
      } catch {
        // best-effort
      }
    }
  }
}
