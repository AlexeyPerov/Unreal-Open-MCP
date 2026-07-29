// Tests for `configure.ts` — project-local settings read/write + merge.
//
// Built + run via the package test config (see package.json `test`):
//   tsc -p tsconfig.test.json  &&  node --test 'dist-test/**/*.test.js'

import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";

import {
  settingsDir,
  settingsPath,
  readSettings,
  writeSettings,
  mergeSettings,
  type ProjectSettings,
} from "./configure.js";

/** Fresh temp project dir; cleaned up at the end of each test. */
function freshProject(): string {
  return fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-configure-"));
}

/** Read + parse the settings file directly (test oracle). */
function readRaw(p: string): ProjectSettings {
  return JSON.parse(fs.readFileSync(p, "utf8")) as ProjectSettings;
}

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------

test("settingsDir is <project>/.unreal-open-mcp", () => {
  assert.equal(settingsDir("/proj/MyGame"), "/proj/MyGame/.unreal-open-mcp");
});

test("settingsPath is <project>/.unreal-open-mcp/settings.json", () => {
  assert.equal(settingsPath("/proj/MyGame"), "/proj/MyGame/.unreal-open-mcp/settings.json");
});

// ---------------------------------------------------------------------------
// readSettings
// ---------------------------------------------------------------------------

test("readSettings: missing file => empty object, missing:true", () => {
  const proj = freshProject();
  try {
    const r = readSettings(proj);
    assert.equal(r.ok, true);
    if (r.ok) {
      assert.deepEqual(r.settings, {});
      assert.equal(r.missing, true);
      assert.equal(r.malformed, false);
    }
  } finally {
    fs.rmSync(proj, { recursive: true, force: true });
  }
});

test("readSettings: parses an existing file", () => {
  const proj = freshProject();
  try {
    const p = settingsPath(proj);
    fs.mkdirSync(settingsDir(proj), { recursive: true });
    fs.writeFileSync(p, JSON.stringify({ bridgePort: 27123, authMode: "none" }), "utf8");
    const r = readSettings(proj);
    assert.equal(r.ok, true);
    if (r.ok) {
      assert.equal(r.settings.bridgePort, 27123);
      assert.equal(r.settings.authMode, "none");
      assert.equal(r.missing, false);
      assert.equal(r.malformed, false);
    }
  } finally {
    fs.rmSync(proj, { recursive: true, force: true });
  }
});

test("readSettings: malformed JSON => empty object, malformed:true", () => {
  const proj = freshProject();
  try {
    const p = settingsPath(proj);
    fs.mkdirSync(settingsDir(proj), { recursive: true });
    fs.writeFileSync(p, "{ not json", "utf8");
    const r = readSettings(proj);
    assert.equal(r.ok, true);
    if (r.ok) {
      assert.deepEqual(r.settings, {});
      assert.equal(r.malformed, true);
    }
  } finally {
    fs.rmSync(proj, { recursive: true, force: true });
  }
});

test("readSettings: non-object JSON (array) => malformed", () => {
  const proj = freshProject();
  try {
    const p = settingsPath(proj);
    fs.mkdirSync(settingsDir(proj), { recursive: true });
    fs.writeFileSync(p, "[1,2,3]", "utf8");
    const r = readSettings(proj);
    assert.equal(r.ok, true);
    if (r.ok) assert.equal(r.malformed, true);
  } finally {
    fs.rmSync(proj, { recursive: true, force: true });
  }
});

test("readSettings: project dir missing => error", () => {
  const r = readSettings("/nonexistent/uomcp/configure/test");
  assert.equal(r.ok, false);
  if (!r.ok) assert.equal(r.kind, "project_dir_missing");
});

test("readSettings: project path is a file => error", () => {
  const tmp = path.join(os.tmpdir(), "uomcp-configure-file");
  fs.writeFileSync(tmp, "x", "utf8");
  try {
    const r = readSettings(tmp);
    assert.equal(r.ok, false);
    if (!r.ok) assert.equal(r.kind, "project_dir_not_dir");
  } finally {
    fs.unlinkSync(tmp);
  }
});

// ---------------------------------------------------------------------------
// mergeSettings
// ---------------------------------------------------------------------------

test("mergeSettings: overwrites scalar keys", () => {
  const merged = mergeSettings({ bridgePort: 1, authMode: "none" }, { bridgePort: 2 });
  assert.equal(merged.bridgePort, 2);
  assert.equal(merged.authMode, "none");
});

test("mergeSettings: null patch value deletes the key", () => {
  const merged = mergeSettings({ bridgePort: 1, authMode: "none" }, { bridgePort: null });
  assert.equal("bridgePort" in merged, false);
  assert.equal(merged.authMode, "none");
});

test("mergeSettings: deep-merges nested plain objects", () => {
  const merged = mergeSettings(
    { nested: { a: 1, b: 2 } } as ProjectSettings,
    { nested: { b: 3, c: 4 } } as ProjectSettings,
  );
  assert.deepEqual(merged.nested, { a: 1, b: 3, c: 4 });
});

test("mergeSettings: preserves unrelated keys", () => {
  const merged = mergeSettings(
    { bridgePort: 1, authMode: "none", defaultGateMode: "enforce" },
    { bridgePort: 2 },
  );
  assert.equal(merged.defaultGateMode, "enforce");
});

// ---------------------------------------------------------------------------
// writeSettings (round-trip + idempotency)
// ---------------------------------------------------------------------------

test("writeSettings: creates the settings dir + file on first run", () => {
  const proj = freshProject();
  try {
    const r = writeSettings(proj, { bridgePort: 27123 });
    assert.equal(r.ok, true);
    if (r.ok) {
      assert.equal(r.createdDir, true);
      assert.equal(r.settings.bridgePort, 27123);
      assert.ok(fs.existsSync(settingsPath(proj)));
      assert.equal(readRaw(settingsPath(proj)).bridgePort, 27123);
    }
  } finally {
    fs.rmSync(proj, { recursive: true, force: true });
  }
});

test("writeSettings: round-trip set then clear bridge port", () => {
  const proj = freshProject();
  try {
    const set = writeSettings(proj, { bridgePort: 27123 });
    assert.equal(set.ok, true);
    assert.equal(readRaw(settingsPath(proj)).bridgePort, 27123);

    const clear = writeSettings(proj, { bridgePort: null });
    assert.equal(clear.ok, true);
    assert.equal("bridgePort" in readRaw(settingsPath(proj)), false);
  } finally {
    fs.rmSync(proj, { recursive: true, force: true });
  }
});

test("writeSettings: preserves unrelated keys across a partial update", () => {
  const proj = freshProject();
  try {
    // Seed with a bridge-written key the CLI does not own.
    fs.mkdirSync(settingsDir(proj), { recursive: true });
    fs.writeFileSync(
      settingsPath(proj),
      JSON.stringify({ authMode: "none", defaultGateMode: "enforce" }),
      "utf8",
    );
    const r = writeSettings(proj, { bridgePort: 9999 });
    assert.equal(r.ok, true);
    const onDisk = readRaw(settingsPath(proj));
    assert.equal(onDisk.bridgePort, 9999);
    assert.equal(onDisk.authMode, "none");
    assert.equal(onDisk.defaultGateMode, "enforce");
  } finally {
    fs.rmSync(proj, { recursive: true, force: true });
  }
});

test("writeSettings: idempotent re-write is a no-op on disk", () => {
  const proj = freshProject();
  try {
    writeSettings(proj, { bridgePort: 27123 });
    const before = fs.readFileSync(settingsPath(proj), "utf8");
    writeSettings(proj, { bridgePort: 27123 });
    const after = fs.readFileSync(settingsPath(proj), "utf8");
    assert.equal(before, after);
  } finally {
    fs.rmSync(proj, { recursive: true, force: true });
  }
});

test("writeSettings: createdDir is false when the dir already exists", () => {
  const proj = freshProject();
  try {
    fs.mkdirSync(settingsDir(proj), { recursive: true });
    const r = writeSettings(proj, { bridgePort: 1 });
    assert.equal(r.ok, true);
    if (r.ok) assert.equal(r.createdDir, false);
  } finally {
    fs.rmSync(proj, { recursive: true, force: true });
  }
});
