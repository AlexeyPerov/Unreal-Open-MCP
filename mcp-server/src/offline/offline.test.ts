// Unit tests for the P8.7 offline read surfaces:
//   - editor-log.ts (path resolve + tail reader)
//   - compile-errors.ts (MSVC + clang diagnostic parser)
//   - source-read.ts (Source/ jail + offline reader)
//   - project-index.ts (.uproject parse + file listing)
//
// All tests use a temp project dir with a fake `.uproject`, `Source/Foo.cpp`,
// and `Saved/Logs/MyProject.log` planted on disk — no Unreal, no editor, no
// bridge. The bridge being down is the WHOLE POINT of these readers, so the
// LiveClient is never consulted.

import test from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, writeFileSync, rmSync, symlinkSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import {
  resolveEditorLogPath,
  discoverLogFiles,
  readLogTail,
  editorLogsDir,
  DEFAULT_LOG_TAIL_BYTES,
  MIN_LOG_TAIL_BYTES,
  MAX_LOG_TAIL_BYTES,
} from "./editor-log.js";
import {
  extractCompileErrors,
  summarizeProjectHealth,
  compileErrorStatus,
  MAX_COMPILE_ERRORS,
} from "./compile-errors.js";
import {
  resolveJailedSourcePath,
  readSourceOffline,
} from "./source-read.js";
import {
  parseUProject,
  listProjectFiles,
  buildProjectIndex,
} from "./project-index.js";
import { ToolRouter, routePolicy } from "../tool-router.js";
import type { Router, CallToolResult } from "../tool-router.js";

/** Build a temp project dir with the standard P8.7 layout. Returns the project
 *  root path and a cleanup handle. */
function makeTempProject(prefix = "uomcp-offline-"): {
  root: string;
  cleanup: () => void;
} {
  const root = mkdtempSync(join(tmpdir(), prefix));
  mkdirSync(join(root, "Saved", "Logs"), { recursive: true });
  mkdirSync(join(root, "Source", "MyGame"), { recursive: true });
  mkdirSync(join(root, "Config"), { recursive: true });
  return {
    root,
    cleanup: () => {
      try {
        rmSync(root, { recursive: true, force: true });
      } catch {
        // best-effort
      }
    },
  };
}

/** Parse the first text content block of a CallToolResult as JSON. */
function bodyOf(result: CallToolResult): Record<string, unknown> {
  const block = result.content[0];
  assert.ok(block?.type === "text", "first content block must be text");
  assert.ok(typeof block.text === "string");
  return JSON.parse(block.text);
}

/** A stub live transport that records calls — the offline tools must NEVER
 *  reach it. */
function makeStubLive(): Router & { calls: unknown[] } {
  const calls: unknown[] = [];
  return {
    calls,
    async route() {
      calls.push("REACHED_LIVE");
      return {
        content: [{ type: "text", text: JSON.stringify({ shouldNotReach: true }) }],
        isError: false,
      };
    },
  };
}

// ---------------------------------------------------------------------------
// editor-log.ts — path resolution + tail reader
// ---------------------------------------------------------------------------

test("editorLogsDir is <project>/Saved/Logs", () => {
  assert.equal(editorLogsDir("/proj/MyGame"), join("/proj/MyGame", "Saved", "Logs"));
});

test("resolveEditorLogPath returns null when Saved/Logs is absent", () => {
  const { root, cleanup } = makeTempProject();
  try {
    rmSync(join(root, "Saved", "Logs"), { recursive: true, force: true });
    assert.equal(resolveEditorLogPath(root), null);
  } finally {
    cleanup();
  }
});

test("resolveEditorLogPath returns the newest .log by mtime", async () => {
  const { root, cleanup } = makeTempProject();
  try {
    const logsDir = join(root, "Saved", "Logs");
    const old = join(logsDir, "MyProject.log");
    const newer = join(logsDir, "MyProject-2026.07.29-10.00.00.log");
    writeFileSync(old, "old content");
    // Ensure the newer file has a strictly greater mtime than the old one.
    const future = new Date(Date.now() + 5000);
    writeFileSync(newer, "newer content");
    try {
      // bump mtime explicitly to guarantee ordering across filesystems
      const { utimesSync } = await import("node:fs");
      utimesSync(old, future, new Date(Date.now() - 5000));
      utimesSync(newer, future, future);
    } catch {
      // utimes best-effort; the write-order fallback still holds
    }
    const resolved = resolveEditorLogPath(root);
    assert.equal(resolved, newer);
  } finally {
    cleanup();
  }
});

test("discoverLogFiles ignores non-.log files", () => {
  const { root, cleanup } = makeTempProject();
  try {
    const logsDir = join(root, "Saved", "Logs");
    writeFileSync(join(logsDir, "MyProject.log"), "x");
    writeFileSync(join(logsDir, "notes.txt"), "y");
    writeFileSync(join(logsDir, "backup.bak"), "z");
    const logs = discoverLogFiles(root);
    assert.equal(logs.length, 1);
    assert.ok(logs[0].endsWith("MyProject.log"));
  } finally {
    cleanup();
  }
});

test("readLogTail returns exists:false for a missing file", () => {
  const result = readLogTail(join(tmpdir(), "does-not-exist-uomcp.log"));
  assert.equal(result.exists, false);
  assert.equal(result.bytes, 0);
  assert.equal(result.content, "");
});

test("readLogTail reads the last maxBytes bytes (clamped up to MIN)", () => {
  const { root, cleanup } = makeTempProject();
  try {
    const file = join(root, "Saved", "Logs", "Tail.log");
    // A file larger than MIN_LOG_TAIL_BYTES so the clamp does not swallow the
    // whole file: head of 'A' + a distinct 'B'-marker tail. Tail the marker
    // size (which is >= MIN) so the result is all marker.
    const marker = "B".repeat(MIN_LOG_TAIL_BYTES);
    writeFileSync(file, "A".repeat(MIN_LOG_TAIL_BYTES * 2) + marker);
    const result = readLogTail(file, marker.length);
    assert.equal(result.exists, true);
    assert.equal(result.bytes, marker.length);
    assert.equal(result.content, marker);
  } finally {
    cleanup();
  }
});

test("readLogTail returns the whole file when it is smaller than maxBytes", () => {
  const { root, cleanup } = makeTempProject();
  try {
    const file = join(root, "Saved", "Logs", "Small.log");
    writeFileSync(file, "hello world");
    const result = readLogTail(file, DEFAULT_LOG_TAIL_BYTES);
    assert.equal(result.content, "hello world");
    assert.equal(result.bytes, "hello world".length);
  } finally {
    cleanup();
  }
});

test("readLogTail clamps maxBytes to [MIN, MAX]", () => {
  const { root, cleanup } = makeTempProject();
  try {
    const file = join(root, "Saved", "Logs", "Clamp.log");
    writeFileSync(file, "x".repeat(MAX_LOG_TAIL_BYTES + 100));
    // A tail larger than MAX is clamped down to MAX.
    const result = readLogTail(file, MAX_LOG_TAIL_BYTES + 1000);
    assert.ok(result.bytes <= MAX_LOG_TAIL_BYTES, "must not exceed MAX_LOG_TAIL_BYTES");
    assert.ok(result.bytes >= MIN_LOG_TAIL_BYTES, "must not go below MIN_LOG_TAIL_BYTES");
  } finally {
    cleanup();
  }
});

// ---------------------------------------------------------------------------
// compile-errors.ts — MSVC + clang diagnostic parser
// ---------------------------------------------------------------------------

test("extractCompileErrors parses MSVC error + warning lines", () => {
  const log =
    "MyGame/MyActor.cpp(42): error C2065: 'Foo': undeclared identifier\n" +
    "MyGame/Other.cpp(7,3): warning C4101: 'X': unreferenced local variable\n";
  const errors = extractCompileErrors(log);
  assert.equal(errors.length, 2);
  assert.equal(errors[0].file, "MyGame/MyActor.cpp");
  assert.equal(errors[0].line, 42);
  assert.equal(errors[0].severity, "error");
  assert.equal(errors[0].message, "C2065: 'Foo': undeclared identifier");
  assert.equal(errors[1].file, "MyGame/Other.cpp");
  assert.equal(errors[1].line, 7);
  assert.equal(errors[1].severity, "warning");
});

test("extractCompileErrors parses clang error + fatal-error lines (normalized to error)", () => {
  const log =
    "MyGame/MyActor.cpp:42:5: error: use of undeclared identifier 'Foo'\n" +
    "MyGame/Other.cpp:1:10: fatal error: 'Missing.h' file not found\n";
  const errors = extractCompileErrors(log);
  assert.equal(errors.length, 2);
  assert.equal(errors[0].file, "MyGame/MyActor.cpp");
  assert.equal(errors[0].line, 42);
  assert.equal(errors[0].severity, "error");
  assert.equal(errors[1].severity, "error", "fatal error → severity error");
});

test("extractCompileErrors dedupes identical diagnostics", () => {
  const line = "MyGame/MyActor.cpp(42): error C2065: 'Foo': undeclared identifier\n";
  const errors = extractCompileErrors(line + line + line);
  assert.equal(errors.length, 1);
});

test("extractCompileErrors ignores bare LINK : fatal rows", () => {
  const log =
    "LINK : fatal error LNK1104: cannot open file 'MyGame.lib'\n" +
    "MyGame/MyActor.cpp(1): error C2065: 'X': undeclared identifier\n";
  const errors = extractCompileErrors(log);
  assert.equal(errors.length, 1, "LINK rows are link-stage, not emitted");
  assert.equal(errors[0].file, "MyGame/MyActor.cpp");
});

test("extractCompileErrors returns empty for a clean log", () => {
  const log =
    "LogInit: Display: Running engine...\n" +
    "LogCore: Nothing to see here\n";
  assert.deepEqual(extractCompileErrors(log), []);
});

test("extractCompileErrors is capped at MAX_COMPILE_ERRORS", () => {
  let log = "";
  for (let i = 0; i < MAX_COMPILE_ERRORS + 20; i++) {
    log += `F${i}.cpp(1): error C2065: 'x' undeclared identifier ${i}\n`;
  }
  const errors = extractCompileErrors(log);
  assert.equal(errors.length, MAX_COMPILE_ERRORS);
});

test("summarizeProjectHealth: errors make it unhealthy, warnings alone do not", () => {
  const withError = summarizeProjectHealth(
    "F.cpp(1): error C2065: 'x'\n",
  );
  assert.equal(withError.unhealthy, true);
  assert.equal(withError.errors.length, 1);
  assert.ok(withError.headline.includes("1 compile error"));

  const withWarningOnly = summarizeProjectHealth(
    "F.cpp(1): warning C4101: 'x'\n",
  );
  assert.equal(withWarningOnly.unhealthy, false);
  assert.equal(withWarningOnly.headline, "");
  assert.equal(withWarningOnly.errors.length, 1, "warning still surfaces in errors[]");
});

test("compileErrorStatus maps health + logFound to the status token", () => {
  const health = summarizeProjectHealth("F.cpp(1): error C2065: 'x'\n");
  assert.equal(compileErrorStatus(health, true), "compile_failed");
  const clean = summarizeProjectHealth("");
  assert.equal(compileErrorStatus(clean, true), "no_errors_found");
  assert.equal(compileErrorStatus(null, false), "log_not_found");
});

// ---------------------------------------------------------------------------
// source-read.ts — Source/ jail + offline reader
// ---------------------------------------------------------------------------

test("resolveJailedSourcePath accepts a relative path under Source/", () => {
  const { root, cleanup } = makeTempProject();
  try {
    const r = resolveJailedSourcePath(root, "MyGame/MyActor.cpp");
    assert.equal(r.ok, true);
    if (r.ok) {
      assert.ok(r.absolute.endsWith(join("Source", "MyGame", "MyActor.cpp")));
      assert.equal(r.relative, join("MyGame", "MyActor.cpp"));
    }
  } finally {
    cleanup();
  }
});

test("resolveJailedSourcePath accepts an absolute path inside Source/", () => {
  const { root, cleanup } = makeTempProject();
  try {
    const abs = join(root, "Source", "MyGame", "MyActor.cpp");
    const r = resolveJailedSourcePath(root, abs);
    assert.equal(r.ok, true);
  } finally {
    cleanup();
  }
});

test("resolveJailedSourcePath rejects '..' traversal escapes", () => {
  const { root, cleanup } = makeTempProject();
  try {
    const cases = [
      "../Secret.cpp",
      "MyGame/../../Secret.cpp",
      "..\\..\\Secret.cpp", // backslash form
    ];
    for (const c of cases) {
      const r = resolveJailedSourcePath(root, c);
      assert.equal(r.ok, false, `${c} should be rejected`);
      if (!r.ok) assert.equal(r.code, "path_escapes_jail");
    }
  } finally {
    cleanup();
  }
});

test("resolveJailedSourcePath rejects absolute-outside and NTFS ADS (':') escapes", () => {
  const { root, cleanup } = makeTempProject();
  try {
    const absOutside = join(tmpdir(), "outside-uomcp", "Secret.cpp");
    let r = resolveJailedSourcePath(root, absOutside);
    assert.equal(r.ok, false, "absolute-outside should be rejected");

    r = resolveJailedSourcePath(root, "MyGame/MyActor.cpp:stream");
    assert.equal(r.ok, false, "NTFS ADS (':') should be rejected");
    if (!r.ok) assert.equal(r.code, "path_escapes_jail");
  } finally {
    cleanup();
  }
});

test("readSourceOffline reads a planted source file with numbered lines", () => {
  const { root, cleanup } = makeTempProject();
  try {
    const file = join(root, "Source", "MyGame", "Foo.cpp");
    writeFileSync(file, "line1\nline2\nline3\n");
    const outcome = readSourceOffline(root, "MyGame/Foo.cpp");
    assert.equal(outcome.ok, true);
    if (outcome.ok) {
      assert.equal(outcome.result.total_lines, 3);
      assert.equal(outcome.result.start_line, 1);
      assert.equal(outcome.result.end_line, 3);
      assert.equal(outcome.result.truncated, false);
      assert.deepEqual(outcome.result.lines, [
        { line: 1, text: "line1" },
        { line: 2, text: "line2" },
        { line: 3, text: "line3" },
      ]);
    }
  } finally {
    cleanup();
  }
});

test("readSourceOffline honors start_line / end_line windowing", () => {
  const { root, cleanup } = makeTempProject();
  try {
    const file = join(root, "Source", "MyGame", "Foo.cpp");
    writeFileSync(file, "l1\nl2\nl3\nl4\nl5\n");
    const outcome = readSourceOffline(root, "MyGame/Foo.cpp", {
      start_line: 2,
      end_line: 4,
    });
    assert.equal(outcome.ok, true);
    if (outcome.ok) {
      assert.equal(outcome.result.start_line, 2);
      assert.equal(outcome.result.end_line, 4);
      assert.deepEqual(
        outcome.result.lines.map((l) => l.line),
        [2, 3, 4],
      );
    }
  } finally {
    cleanup();
  }
});

test("readSourceOffline truncates + flags when window exceeds max_lines", () => {
  const { root, cleanup } = makeTempProject();
  try {
    const file = join(root, "Source", "MyGame", "Foo.cpp");
    let content = "";
    for (let i = 1; i <= 50; i++) content += `l${i}\n`;
    writeFileSync(file, content);
    const outcome = readSourceOffline(root, "MyGame/Foo.cpp", { max_lines: 10 });
    assert.equal(outcome.ok, true);
    if (outcome.ok) {
      assert.equal(outcome.result.truncated, true);
      assert.equal(outcome.result.lines.length, 10);
      assert.equal(outcome.result.end_line, 10);
    }
  } finally {
    cleanup();
  }
});

test("readSourceOffline surfaces structured error codes", () => {
  const { root, cleanup } = makeTempProject();
  try {
    // path_escapes_jail
    let o = readSourceOffline(root, "../escape.cpp");
    assert.equal(o.ok, false);
    if (!o.ok) assert.equal(o.code, "path_escapes_jail");

    // file_not_found
    o = readSourceOffline(root, "MyGame/Missing.cpp");
    assert.equal(o.ok, false);
    if (!o.ok) assert.equal(o.code, "file_not_found");

    // not_a_file (directory)
    o = readSourceOffline(root, "MyGame");
    assert.equal(o.ok, false);
    if (!o.ok) assert.equal(o.code, "not_a_file");
  } finally {
    cleanup();
  }
});

// ---------------------------------------------------------------------------
// project-index.ts — .uproject parse + file listing
// ---------------------------------------------------------------------------

const UPROJECT_FIXTURE = JSON.stringify({
  FileVersion: 5,
  EngineAssociation: "5.8",
  Category: "Project",
  Description: "Test project",
  Modules: [
    { Name: "MyGame", Type: "Runtime", LoadingPhase: "Default" },
    { Name: "MyGameEditor", Type: "Editor", LoadingPhase: "PostEngineInit" },
  ],
  Plugins: [{ Name: "UnrealOpenMCP", Enabled: true }],
});

test("parseUProject parses engine association, modules, and plugins", () => {
  const { root, cleanup } = makeTempProject();
  try {
    writeFileSync(join(root, "uoproj-temp.uproject"), UPROJECT_FIXTURE);
    const desc = parseUProject(root);
    assert.equal(desc.found, true);
    assert.equal(desc.engine_association, "5.8");
    assert.equal(desc.category, "Project");
    assert.equal(desc.modules.length, 2);
    assert.equal(desc.modules[0].name, "MyGame");
    assert.equal(desc.modules[0].type, "Runtime");
    assert.equal(desc.modules[1].loading_phase, "PostEngineInit");
    assert.deepEqual(desc.plugins, ["UnrealOpenMCP"]);
  } finally {
    cleanup();
  }
});

test("parseUProject returns found:false when no .uproject exists", () => {
  const { root, cleanup } = makeTempProject();
  try {
    const desc = parseUProject(root);
    assert.equal(desc.found, false);
    assert.deepEqual(desc.modules, []);
  } finally {
    cleanup();
  }
});

test("parseUProject surfaces parse_error on a malformed .uproject", () => {
  const { root, cleanup } = makeTempProject();
  try {
    writeFileSync(join(root, "broken.uproject"), "{ not valid json");
    const desc = parseUProject(root);
    assert.equal(desc.found, true);
    assert.ok(desc.parse_error);
  } finally {
    cleanup();
  }
});

test("listProjectFiles lists Source text extensions, excludes binaries", () => {
  const { root, cleanup } = makeTempProject();
  try {
    writeFileSync(join(root, "Source", "MyGame", "Foo.cpp"), "x");
    writeFileSync(join(root, "Source", "MyGame", "Foo.h"), "x");
    writeFileSync(join(root, "Source", "MyGame", "MyGame.Build.cs"), "x");
    writeFileSync(join(root, "Source", "MyGame", "Texture.uasset"), "binary");
    const list = listProjectFiles(root, "Source");
    const paths = list.files.map((f) => f.path);
    assert.ok(paths.some((p) => p.endsWith("Foo.cpp")));
    assert.ok(paths.some((p) => p.endsWith("Foo.h")));
    assert.ok(paths.some((p) => p.endsWith("MyGame.Build.cs")));
    assert.ok(
      !paths.some((p) => p.endsWith(".uasset")),
      ".uasset must NEVER be listed (ADR-006)",
    );
  } finally {
    cleanup();
  }
});

test("listProjectFiles refuses a non-allow-listed root", () => {
  const { root, cleanup } = makeTempProject();
  try {
    const list = listProjectFiles(root, "Binaries");
    assert.deepEqual(list.files, []);
  } finally {
    cleanup();
  }
});

test("listProjectFiles truncates + flags when exceeding max_files", () => {
  const { root, cleanup } = makeTempProject();
  try {
    for (let i = 0; i < 10; i++) {
      writeFileSync(join(root, "Source", "MyGame", `F${i}.cpp`), "x");
    }
    const list = listProjectFiles(root, "Source", { max_files: 3 });
    assert.equal(list.files.length, 3);
    assert.equal(list.truncated, true);
  } finally {
    cleanup();
  }
});

test("listProjectFiles skips a symlink/junction that escapes the project root", () => {
  const { root, cleanup } = makeTempProject();
  // Skip on platforms where unprivileged symlinks are not allowed.
  const outside = mkdtempSync(join(tmpdir(), "uomcp-outside-"));
  try {
    writeFileSync(join(outside, "Secret.cpp"), "leaked");
    try {
      symlinkSync(outside, join(root, "Source", "escape-link"), "dir");
    } catch {
      // symlink creation may fail without privileges; skip the test gracefully.
      return;
    }
    // Place a real file so the listing is not empty when the link is skipped.
    writeFileSync(join(root, "Source", "MyGame", "Real.cpp"), "x");
    const list = listProjectFiles(root, "Source");
    const paths = list.files.map((f) => f.path);
    assert.ok(
      !paths.some((p) => p.includes("escape-link") && p.includes("Secret")),
      "a symlink escaping the project must not surface its target",
    );
  } finally {
    cleanup();
    rmSync(outside, { recursive: true, force: true });
  }
});

test("buildProjectIndex composes .uproject + optional file list", () => {
  const { root, cleanup } = makeTempProject();
  try {
    writeFileSync(join(root, "p-temp.uproject"), UPROJECT_FIXTURE);
    writeFileSync(join(root, "Source", "MyGame", "Foo.cpp"), "x");
    const idx = buildProjectIndex(root, { list: "Source" });
    assert.equal(idx.uproject.found, true);
    assert.equal(idx.uproject.engine_association, "5.8");
    assert.ok(idx.file_list);
    assert.ok(idx.file_list!.files.some((f) => f.path.endsWith("Foo.cpp")));
  } finally {
    cleanup();
  }
});

// ---------------------------------------------------------------------------
// Router offline routes — the three tools resolve from disk, never hit live
// ---------------------------------------------------------------------------

test("routePolicy classifies the three offline tools as offline", () => {
  assert.equal(routePolicy("unreal_open_mcp_read_compile_errors"), "offline");
  assert.equal(routePolicy("unreal_open_mcp_source_read_offline"), "offline");
  assert.equal(routePolicy("unreal_open_mcp_project_index"), "offline");
});

test("read_compile_errors returns structured diagnostics from a planted log, never hits live", async () => {
  const { root, cleanup } = makeTempProject();
  try {
    writeFileSync(join(root, "p-temp.uproject"), "{}");
    writeFileSync(
      join(root, "Saved", "Logs", "MyProject.log"),
      "LogInit: starting\n" +
        "MyGame/MyActor.cpp(42): error C2065: 'Foo': undeclared identifier\n",
    );
    const live = makeStubLive();
    const router = new ToolRouter(live, root);
    const result = await router.route("unreal_open_mcp_read_compile_errors", {});
    assert.deepEqual(live.calls, [], "must NOT route through the live transport");
    assert.equal(result.isError, false);
    const body = bodyOf(result);
    assert.equal(body._source, "offline");
    assert.deepEqual(body._route, { route: "offline" });
    assert.equal(body.status, "compile_failed");
    assert.equal(body.unhealthy, true);
    assert.equal(body.error_count, 1);
    const errors = body.errors as Array<{ file: string; line: number; severity: string }>;
    assert.equal(errors[0].file, "MyGame/MyActor.cpp");
    assert.equal(errors[0].line, 42);
    assert.equal(errors[0].severity, "error");
  } finally {
    cleanup();
  }
});

test("read_compile_errors returns log_not_found (non-error) when Saved/Logs is empty", async () => {
  const { root, cleanup } = makeTempProject();
  try {
    writeFileSync(join(root, "p-temp.uproject"), "{}");
    rmSync(join(root, "Saved", "Logs"), { recursive: true, force: true });
    const router = new ToolRouter(makeStubLive(), root);
    const result = await router.route("unreal_open_mcp_read_compile_errors", {});
    assert.equal(result.isError, false);
    const body = bodyOf(result);
    assert.equal(body.status, "log_not_found");
    assert.equal(body.unhealthy, false);
    assert.equal(body._source, "offline");
  } finally {
    cleanup();
  }
});

test("source_read_offline reads a Source/ file offline with route metadata", async () => {
  const { root, cleanup } = makeTempProject();
  try {
    writeFileSync(join(root, "Source", "MyGame", "Foo.cpp"), "a\nb\n");
    const live = makeStubLive();
    const router = new ToolRouter(live, root);
    const result = await router.route("unreal_open_mcp_source_read_offline", {
      path: "MyGame/Foo.cpp",
    });
    assert.deepEqual(live.calls, [], "must NOT route through the live transport");
    assert.equal(result.isError, false);
    const body = bodyOf(result);
    assert.equal(body._source, "offline");
    assert.equal(body.total_lines, 2);
    assert.equal(body.truncated, false);
  } finally {
    cleanup();
  }
});

test("source_read_offline rejects a jail escape with path_escapes_jail (offline-tagged)", async () => {
  const { root, cleanup } = makeTempProject();
  try {
    const router = new ToolRouter(makeStubLive(), root);
    const result = await router.route("unreal_open_mcp_source_read_offline", {
      path: "../escape.cpp",
    });
    assert.equal(result.isError, true);
    const body = bodyOf(result);
    assert.equal(body._source, "offline");
    assert.equal((body.error as { code: string }).code, "path_escapes_jail");
  } finally {
    cleanup();
  }
});

test("source_read_offline refuses a missing path with missing_parameter", async () => {
  const { root, cleanup } = makeTempProject();
  try {
    const router = new ToolRouter(makeStubLive(), root);
    const result = await router.route("unreal_open_mcp_source_read_offline", {});
    assert.equal(result.isError, true);
    const body = bodyOf(result);
    assert.equal((body.error as { code: string }).code, "missing_parameter");
  } finally {
    cleanup();
  }
});

test("project_index returns .uproject basics + optional Source list offline", async () => {
  const { root, cleanup } = makeTempProject();
  try {
    writeFileSync(join(root, "pi-temp.uproject"), UPROJECT_FIXTURE);
    writeFileSync(join(root, "Source", "MyGame", "Foo.cpp"), "x");
    const live = makeStubLive();
    const router = new ToolRouter(live, root);
    const result = await router.route("unreal_open_mcp_project_index", {
      list: "Source",
    });
    assert.deepEqual(live.calls, [], "must NOT route through the live transport");
    assert.equal(result.isError, false);
    const body = bodyOf(result);
    assert.equal(body._source, "offline");
    const up = body.uproject as { engine_association: string; modules: unknown[] };
    assert.equal(up.engine_association, "5.8");
    assert.equal(up.modules.length, 2);
    assert.ok(body.file_list, "file_list present when list arg given");
  } finally {
    cleanup();
  }
});

test("project_index refuses an out-of-allow-list list root", async () => {
  const { root, cleanup } = makeTempProject();
  try {
    const router = new ToolRouter(makeStubLive(), root);
    const result = await router.route("unreal_open_mcp_project_index", {
      list: "Binaries",
    });
    assert.equal(result.isError, true);
    const body = bodyOf(result);
    assert.equal((body.error as { code: string }).code, "invalid_parameter");
  } finally {
    cleanup();
  }
});

test("offline tools refuse with project_path_not_bound when no project is bound", async () => {
  const router = new ToolRouter(makeStubLive(), null);
  const result = await router.route("unreal_open_mcp_read_compile_errors", {});
  assert.equal(result.isError, true);
  const body = bodyOf(result);
  assert.equal(body._source, "offline");
  assert.equal((body.error as { code: string }).code, "project_path_not_bound");
});
