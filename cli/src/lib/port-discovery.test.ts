// Tests for `port-discovery.ts` — the thin CLI-side mirror of the mcp-server's
// instance-discovery. Pinned cross-side vectors MUST match
// mcp-server/src/instance-discovery.test.ts AND the bridge-side
// UnrealOpenMcpPortResolverSpec exactly — that is the whole point of
// deterministic per-project ports. If any of these break, update all three
// sides together.
//
// Built + run via the package test config (see package.json `test`):
//   tsc -p tsconfig.test.json  &&  node --test 'dist-test/**/*.test.js'

import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import * as os from "node:os";

import {
  PORT_RANGE_START,
  PORT_RANGE_SIZE,
  HEARTBEAT_STALE_MS,
  normalizePath,
  projectHash,
  computePort,
  resolvePort,
  isUsablePort,
  isPidAlive,
  readInstanceLock,
  lockPath,
  resolveAuthToken,
  authTokenFromLock,
  heartbeatAgeMs,
  classifyInstance,
  statusDir,
  instancesDir,
  type InstanceLock,
  type InstanceClassification,
} from "./port-discovery.js";

// Pinned cross-side values — MUST match mcp-server/src/instance-discovery.test.ts.
const SAMPLE_PATH = "/Users/foo/MyGame";
const SAMPLE_PATH_EXPECTED_PORT = 22028;
const SAMPLE_PATH_EXPECTED_HASH_PREFIX = "dca5061f6f21537c";

const ALT_PATH = "/some/path";
const ALT_PATH_EXPECTED_PORT = 29602;
const ALT_PATH_EXPECTED_HASH_PREFIX = "eda6cf0b63f1a1d2";

// ----- normalizePath --------------------------------------------------------

test("normalizePath replaces backslashes with forward slashes", () => {
  assert.equal(normalizePath("\\Users\\foo\\MyGame"), "/Users/foo/MyGame");
});

test("normalizePath trims trailing slashes", () => {
  assert.equal(normalizePath("/Users/foo/MyGame/"), "/Users/foo/MyGame");
  assert.equal(normalizePath("/Users/foo/MyGame///"), "/Users/foo/MyGame");
});

test("normalizePath keeps a single trailing slash as the root", () => {
  assert.equal(normalizePath("/"), "/");
});

test("normalizePath returns empty string for empty input", () => {
  assert.equal(normalizePath(""), "");
});

test("normalizePath does not lowercase (case-sensitive paths)", () => {
  const mixed = normalizePath("/Users/Foo/MyGame");
  assert.ok(mixed.includes("Foo"));
  assert.ok(mixed.includes("MyGame"));
});

// ----- projectHash ----------------------------------------------------------

test("projectHash is stable across calls", () => {
  assert.equal(projectHash(SAMPLE_PATH), projectHash(SAMPLE_PATH));
});

test("projectHash is lowercase hex SHA256 (64 chars)", () => {
  const hash = projectHash(SAMPLE_PATH);
  assert.equal(hash.length, 64);
  assert.match(hash, /^[0-9a-f]{64}$/);
});

test("projectHash first 8 bytes match the pinned cross-side value", () => {
  assert.equal(projectHash(SAMPLE_PATH).slice(0, 16), SAMPLE_PATH_EXPECTED_HASH_PREFIX);
  assert.equal(projectHash(ALT_PATH).slice(0, 16), ALT_PATH_EXPECTED_HASH_PREFIX);
});

test("projectHash is normalization-stable (backslash + trailing slash hash the same)", () => {
  const forward = projectHash("/Users/foo/MyGame");
  const back = projectHash("\\Users\\foo\\MyGame");
  const trailing = projectHash("/Users/foo/MyGame/");
  assert.equal(forward, back);
  assert.equal(forward, trailing);
});

test("projectHash hashes distinct paths distinctly", () => {
  assert.notEqual(projectHash(SAMPLE_PATH), projectHash(ALT_PATH));
});

// ----- computePort ----------------------------------------------------------

test("computePort stays inside the [20000, 29999] range", () => {
  const port = computePort(SAMPLE_PATH);
  assert.ok(
    port >= PORT_RANGE_START && port < PORT_RANGE_START + PORT_RANGE_SIZE,
    `port ${port} out of range`,
  );
});

test("computePort matches the pinned cross-side value", () => {
  assert.equal(computePort(SAMPLE_PATH), SAMPLE_PATH_EXPECTED_PORT);
  assert.equal(computePort(ALT_PATH), ALT_PATH_EXPECTED_PORT);
});

test("computePort produces distinct ports for distinct paths (pinned samples)", () => {
  assert.notEqual(computePort(SAMPLE_PATH), computePort(ALT_PATH));
});

// ----- isUsablePort ---------------------------------------------------------

test("isUsablePort accepts valid boundaries (1 and 65535)", () => {
  assert.equal(isUsablePort(1), true);
  assert.equal(isUsablePort(65535), true);
});

test("isUsablePort rejects 0, negatives, non-integers, and out-of-range", () => {
  assert.equal(isUsablePort(0), false);
  assert.equal(isUsablePort(-1), false);
  assert.equal(isUsablePort(1.5), false);
  assert.equal(isUsablePort(65536), false);
  assert.equal(isUsablePort("8080"), false);
  assert.equal(isUsablePort(undefined), false);
});

// ----- isPidAlive -----------------------------------------------------------

test("isPidAlive returns false for invalid pids", () => {
  assert.equal(isPidAlive(0), false);
  assert.equal(isPidAlive(-1), false);
});

test("isPidAlive returns true for the current process", () => {
  assert.equal(isPidAlive(process.pid), true);
});

test("isPidAlive returns false for a very-high pid that no OS hands out", () => {
  assert.equal(isPidAlive(4_000_000), false);
});

// ----- paths / dirs ---------------------------------------------------------

test("statusDir is under the home dir", () => {
  assert.equal(statusDir(), path.join(os.homedir(), ".unreal-open-mcp"));
});

test("instancesDir is under statusDir", () => {
  assert.equal(instancesDir(), path.join(statusDir(), "instances"));
});

test("lockPath is <instancesDir>/<hash>.json", () => {
  const p = lockPath(SAMPLE_PATH);
  assert.equal(p, path.join(instancesDir(), `${projectHash(SAMPLE_PATH)}.json`));
});

// ----- resolvePort ----------------------------------------------------------

test("resolvePort: env override wins over everything", () => {
  assert.equal(resolvePort(SAMPLE_PATH, 19120), 19120);
});

test("resolvePort: env override accepts the valid boundaries (1 and 65535)", () => {
  assert.equal(resolvePort(SAMPLE_PATH, 1), 1);
  assert.equal(resolvePort(SAMPLE_PATH, 65535), 65535);
});

test("resolvePort: invalid env override falls through to the hash", () => {
  // isUsablePort rejects these, so resolvePort ignores them.
  assert.equal(resolvePort(SAMPLE_PATH, 0), SAMPLE_PATH_EXPECTED_PORT);
  assert.equal(resolvePort(SAMPLE_PATH, 70000), SAMPLE_PATH_EXPECTED_PORT);
});

test("resolvePort: no lock + no env => deterministic hash", () => {
  // No lock file on disk for SAMPLE_PATH in the test env.
  assert.equal(resolvePort(SAMPLE_PATH, undefined), SAMPLE_PATH_EXPECTED_PORT);
});

test("resolvePort: live lock port wins over the hash", () => {
  const tmp = freshLockFixture(SAMPLE_PATH, {
    pid: process.pid,
    port: 23456,
  });
  try {
    assert.equal(resolvePort(SAMPLE_PATH, undefined), 23456);
  } finally {
    cleanupLockFixture(tmp);
  }
});

test("resolvePort: stale lock (dead pid) falls through to the hash", () => {
  const tmp = freshLockFixture(SAMPLE_PATH, {
    pid: 4_000_000, // dead
    port: 23456,
  });
  try {
    assert.equal(resolvePort(SAMPLE_PATH, undefined), SAMPLE_PATH_EXPECTED_PORT);
  } finally {
    cleanupLockFixture(tmp);
  }
});

test("resolvePort: lock with unusable port falls through to the hash", () => {
  const tmp = freshLockFixture(SAMPLE_PATH, {
    pid: process.pid,
    port: 0, // unusable
  });
  try {
    assert.equal(resolvePort(SAMPLE_PATH, undefined), SAMPLE_PATH_EXPECTED_PORT);
  } finally {
    cleanupLockFixture(tmp);
  }
});

// ----- readInstanceLock / resolveAuthToken ----------------------------------

test("readInstanceLock returns null for a project with no lock file", () => {
  // A path with a vanishingly unlikely pre-existing lock.
  assert.equal(readInstanceLock("/nonexistent/project/for/tests/MyGame"), null);
});

test("readInstanceLock returns the parsed lock when present", () => {
  const tmp = freshLockFixture(SAMPLE_PATH, {
    pid: process.pid,
    port: 23456,
    authToken: "abc123",
  });
  try {
    const lock = readInstanceLock(SAMPLE_PATH);
    assert.ok(lock);
    assert.equal(lock?.port, 23456);
    assert.equal(lock?.authToken, "abc123");
  } finally {
    cleanupLockFixture(tmp);
  }
});

test("resolveAuthToken: undefined when env port override is set", () => {
  assert.equal(resolveAuthToken(SAMPLE_PATH, 19120), undefined);
});

test("resolveAuthToken: token from a live lock", () => {
  const tmp = freshLockFixture(SAMPLE_PATH, {
    pid: process.pid,
    port: 23456,
    authToken: "secret-token",
  });
  try {
    assert.equal(resolveAuthToken(SAMPLE_PATH, undefined), "secret-token");
  } finally {
    cleanupLockFixture(tmp);
  }
});

test("resolveAuthToken: undefined when the lock pid is dead", () => {
  const tmp = freshLockFixture(SAMPLE_PATH, {
    pid: 4_000_000,
    port: 23456,
    authToken: "secret-token",
  });
  try {
    assert.equal(resolveAuthToken(SAMPLE_PATH, undefined), undefined);
  } finally {
    cleanupLockFixture(tmp);
  }
});

test("authTokenFromLock: undefined for null lock", () => {
  assert.equal(authTokenFromLock(null), undefined);
});

test("authTokenFromLock: undefined for an empty token string", () => {
  const lock: InstanceLock = {
    pid: process.pid,
    port: 23456,
    authToken: "",
    projectPath: SAMPLE_PATH,
    projectHash: projectHash(SAMPLE_PATH),
    startedAt: new Date().toISOString(),
    updatedAt: new Date().toISOString(),
    heartbeatAt: new Date().toISOString(),
    state: "idle",
    isPlaying: false,
    isCompiling: false,
    bridgeVersion: "0.0.1",
    unrealVersion: "5.7",
  };
  assert.equal(authTokenFromLock(lock), undefined);
});

// ----- heartbeatAgeMs / classifyInstance ------------------------------------

test("heartbeatAgeMs returns Infinity for a null lock", () => {
  assert.equal(heartbeatAgeMs(null), Infinity);
});

test("heartbeatAgeMs returns Infinity for a missing heartbeatAt", () => {
  const lock = { heartbeatAt: undefined } as unknown as InstanceLock;
  assert.equal(heartbeatAgeMs(lock), Infinity);
});

test("heartbeatAgeMs computes the age against nowMs", () => {
  const lock = { heartbeatAt: new Date(1000).toISOString() } as unknown as InstanceLock;
  assert.equal(heartbeatAgeMs(lock, 6000), 5000);
});

test("classifyInstance: null lock => gone", () => {
  assert.equal(classifyInstance(null), "gone");
});

test("classifyInstance: dead pid => gone", () => {
  const lock = { pid: 4_000_000, heartbeatAt: new Date().toISOString() } as unknown as InstanceLock;
  assert.equal(classifyInstance(lock), "gone");
});

test("classifyInstance: live pid + fresh heartbeat => healthy", () => {
  const lock = {
    pid: process.pid,
    heartbeatAt: new Date().toISOString(),
    state: "idle",
  } as unknown as InstanceLock;
  assert.equal(classifyInstance(lock), "healthy");
});

test("classifyInstance: live pid + compiling state => reloading", () => {
  const lock = {
    pid: process.pid,
    heartbeatAt: new Date().toISOString(),
    state: "compiling",
  } as unknown as InstanceLock;
  assert.equal(classifyInstance(lock), "reloading");
});

test("classifyInstance: live pid + stale heartbeat => dead_bridge", () => {
  const stale = Date.now() - (HEARTBEAT_STALE_MS + 1000);
  const lock = {
    pid: process.pid,
    heartbeatAt: new Date(stale).toISOString(),
    state: "idle",
  } as unknown as InstanceLock;
  const cls: InstanceClassification = classifyInstance(lock);
  assert.equal(cls, "dead_bridge");
});

// ---------------------------------------------------------------------------
// lock-file fixtures
// ---------------------------------------------------------------------------

/**
 * Write a fresh lock file for `projectPath` under the REAL ~/.unreal-open-mcp
 * dir, returning the path so the test can clean it up. The poller reads the
 * real home-dir lock location (the bridge writes there), so the fixture must
 * land there too — we cannot redirect home without env tricks the library does
 * not read. Tests are serialized per-project-hash so there is no write race.
 */
function freshLockFixture(projectPath: string, overrides: Partial<InstanceLock>): string {
  const p = lockPath(projectPath);
  fs.mkdirSync(path.dirname(p), { recursive: true });
  const base: InstanceLock = {
    pid: process.pid,
    port: 20000,
    projectPath,
    projectHash: projectHash(projectPath),
    startedAt: new Date().toISOString(),
    updatedAt: new Date().toISOString(),
    heartbeatAt: new Date().toISOString(),
    state: "idle",
    isPlaying: false,
    isCompiling: false,
    bridgeVersion: "0.0.1",
    unrealVersion: "5.7",
    ...overrides,
  };
  fs.writeFileSync(p, JSON.stringify(base), "utf8");
  return p;
}

function cleanupLockFixture(lockPathStr: string): void {
  try {
    fs.unlinkSync(lockPathStr);
  } catch {
    // best-effort
  }
}
