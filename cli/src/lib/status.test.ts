// Tests for `status.ts` — the pure status-mapping + report-composition surface
// for the `status` CLI command. The deriveBridgeStatus matrix MUST match
// mcp-server/src/tools/bridge-status-helpers.ts exactly (parity test) — that
// is the whole point of CLI + MCP agreeing on the status token.
//
// Built + run via the package test config (see package.json `test`):
//   tsc -p tsconfig.test.json  &&  node --test 'dist-test/**/*.test.js'

import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";

import {
  HEARTBEAT_STALE_MS,
  projectHash,
  type InstanceClassification,
  type InstanceLock,
} from "./port-discovery.js";
import {
  deriveBridgeStatus,
  summarizeInstanceLock,
  detectPlugins,
  composeStatusReport,
  type BridgeStatus,
  type PingProbe,
} from "./status.js";

const SAMPLE_PATH = "/Users/foo/MyGame";

// A fresh, valid lock snapshot for fixture use.
function freshLock(overrides: Partial<InstanceLock> = {}): InstanceLock {
  return {
    pid: process.pid,
    port: 22028,
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
    ...overrides,
  };
}

// ---------------------------------------------------------------------------
// summarizeInstanceLock
// ---------------------------------------------------------------------------

test("summarizeInstanceLock returns null for a null lock", () => {
  assert.equal(summarizeInstanceLock(null), null);
});

test("summarizeInstanceLock returns the operator-facing fields", () => {
  const lock = freshLock({ state: "compiling", isCompiling: true });
  const s = summarizeInstanceLock(lock);
  assert.ok(s);
  assert.equal(s?.pid, lock.pid);
  assert.equal(s?.port, lock.port);
  assert.equal(s?.state, "compiling");
  assert.equal(s?.isCompiling, true);
  assert.equal(s?.isPlaying, false);
  assert.equal(s?.heartbeatAt, lock.heartbeatAt);
  assert.equal(s?.bridgeVersion, lock.bridgeVersion);
  assert.equal(s?.unrealVersion, lock.unrealVersion);
});

// ---------------------------------------------------------------------------
// deriveBridgeStatus — the full parity matrix vs the mcp-server.
// ---------------------------------------------------------------------------

const OK = (o: {
  connected?: boolean;
  compiling?: boolean;
}): Extract<PingProbe, { kind: "ok" }> => ({
  kind: "ok",
  connected: o.connected ?? true,
  compiling: o.compiling,
});

const FAIL: PingProbe = { kind: "fail" };
const NO_PROBE: PingProbe = null;

function derive(
  classification: InstanceClassification,
  ping: PingProbe,
): BridgeStatus {
  return deriveBridgeStatus({ classification, ping });
}

test("status matrix: dead_bridge wins regardless of ping", () => {
  assert.equal(derive("dead_bridge", OK({ connected: true })), "dead_bridge");
  assert.equal(derive("dead_bridge", FAIL), "dead_bridge");
  assert.equal(derive("dead_bridge", NO_PROBE), "dead_bridge");
});

test("status matrix: ping ok + compiling => compiling", () => {
  assert.equal(derive("healthy", OK({ connected: true, compiling: true })), "compiling");
  assert.equal(derive("reloading", OK({ compiling: true })), "compiling");
});

test("status matrix: ping ok + connected => running", () => {
  assert.equal(derive("healthy", OK({ connected: true })), "running");
  assert.equal(derive("reloading", OK({ connected: true })), "running");
});

test("status matrix: ping ok but not connected => stopped", () => {
  // connected:false means the listener is up but not wired to the game thread;
  // the mapper does not have a "connecting" token so it falls through to stopped.
  assert.equal(derive("healthy", OK({ connected: false })), "stopped");
});

test("status matrix: ping fail + reloading => unreachable", () => {
  assert.equal(derive("reloading", FAIL), "unreachable");
  assert.equal(derive("reloading", NO_PROBE), "unreachable");
});

test("status matrix: ping fail + healthy/gone => stopped", () => {
  assert.equal(derive("healthy", FAIL), "stopped");
  assert.equal(derive("gone", FAIL), "stopped");
  assert.equal(derive("gone", NO_PROBE), "stopped");
  assert.equal(derive("healthy", NO_PROBE), "stopped");
});

test("status matrix: no probe mirrors a failed probe (status derived from lock alone)", () => {
  // Only dead_bridge (from the lock) and reloading->unreachable survive without
  // a probe; everything else is stopped.
  assert.equal(derive("healthy", NO_PROBE), derive("healthy", FAIL));
  assert.equal(derive("reloading", NO_PROBE), derive("reloading", FAIL));
  assert.equal(derive("gone", NO_PROBE), derive("gone", FAIL));
});

// ---------------------------------------------------------------------------
// detectPlugins
// ---------------------------------------------------------------------------

test("detectPlugins reports neither installed when Plugins/ is absent", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-status-"));
  try {
    const r = detectPlugins(tmp);
    assert.equal(r.bridgeInstalled, false);
    assert.equal(r.verifyInstalled, false);
    assert.equal(r.bridgeDescriptorPath, null);
    assert.equal(r.verifyDescriptorPath, null);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("detectPlugins reports the bridge installed when its descriptor exists", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-status-"));
  try {
    const bridgeDesc = path.join(tmp, "Plugins", "UnrealOpenMCP", "UnrealOpenMCP.uplugin");
    fs.mkdirSync(path.dirname(bridgeDesc), { recursive: true });
    fs.writeFileSync(bridgeDesc, "{}", "utf8");
    const r = detectPlugins(tmp);
    assert.equal(r.bridgeInstalled, true);
    assert.equal(r.bridgeDescriptorPath, bridgeDesc);
    assert.equal(r.verifyInstalled, false);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("detectPlugins reports both when bridge + verify descriptors exist", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-status-"));
  try {
    const bridgeDesc = path.join(tmp, "Plugins", "UnrealOpenMCP", "UnrealOpenMCP.uplugin");
    const verifyDesc = path.join(tmp, "Plugins", "UnrealOpenMCPVerify", "UnrealOpenMCPVerify.uplugin");
    fs.mkdirSync(path.dirname(bridgeDesc), { recursive: true });
    fs.mkdirSync(path.dirname(verifyDesc), { recursive: true });
    fs.writeFileSync(bridgeDesc, "{}", "utf8");
    fs.writeFileSync(verifyDesc, "{}", "utf8");
    const r = detectPlugins(tmp);
    assert.equal(r.bridgeInstalled, true);
    assert.equal(r.verifyInstalled, true);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

// ---------------------------------------------------------------------------
// composeStatusReport
// ---------------------------------------------------------------------------

test("composeStatusReport: running — live lock + reachable ping", () => {
  const lock = freshLock();
  const report = composeStatusReport({
    projectPath: SAMPLE_PATH,
    port: 22028,
    lock,
    classification: "healthy",
    lockPath: `/home/.unreal-open-mcp/instances/${projectHash(SAMPLE_PATH)}.json`,
    plugins: { bridgeInstalled: true, verifyInstalled: true, bridgeDescriptorPath: "/x", verifyDescriptorPath: "/y" },
    ping: OK({ connected: true }),
    pingBody: { connected: true, bridgeVersion: "0.0.1", unrealVersion: "5.7" },
    probed: true,
  });
  assert.equal(report.command, "status");
  assert.equal(report.projectName, "MyGame");
  assert.equal(report.port, 22028);
  assert.equal(report.baseUrl, "http://127.0.0.1:22028");
  assert.equal(report.bridge.status, "running");
  assert.equal(report.bridge.reachable, true);
  assert.equal(report.bridge.probed, true);
  assert.equal(report.bridge.body?.connected, true);
  assert.equal(report.instance.classification, "healthy");
  assert.equal(report.instance.lock?.pid, process.pid);
});

test("composeStatusReport: stopped — no lock, no probe (editor down)", () => {
  const report = composeStatusReport({
    projectPath: SAMPLE_PATH,
    port: 22028,
    lock: null,
    classification: "gone",
    lockPath: null,
    plugins: { bridgeInstalled: false, verifyInstalled: false, bridgeDescriptorPath: null, verifyDescriptorPath: null },
    ping: null,
    pingBody: null,
    probed: false,
  });
  assert.equal(report.bridge.status, "stopped");
  assert.equal(report.bridge.reachable, false);
  assert.equal(report.bridge.probed, false);
  assert.equal(report.bridge.body, null);
  assert.equal(report.instance.lock, null);
  assert.equal(report.instance.classification, "gone");
});

test("composeStatusReport: unreachable — reloading lock + failed probe", () => {
  const lock = freshLock({ state: "reloading" });
  const report = composeStatusReport({
    projectPath: SAMPLE_PATH,
    port: 22028,
    lock,
    classification: "reloading",
    lockPath: "/x",
    plugins: { bridgeInstalled: true, verifyInstalled: false, bridgeDescriptorPath: "/x", verifyDescriptorPath: null },
    ping: { kind: "fail" },
    pingBody: null,
    probed: true,
  });
  assert.equal(report.bridge.status, "unreachable");
  assert.equal(report.bridge.reachable, false);
  assert.equal(report.bridge.probed, true);
});

test("composeStatusReport: dead_bridge — stale heartbeat surfaces regardless of ping", () => {
  const stale = Date.now() - (HEARTBEAT_STALE_MS + 1000);
  const lock = freshLock({ heartbeatAt: new Date(stale).toISOString() });
  const report = composeStatusReport({
    projectPath: SAMPLE_PATH,
    port: 22028,
    lock,
    classification: "dead_bridge",
    lockPath: "/x",
    plugins: { bridgeInstalled: true, verifyInstalled: true, bridgeDescriptorPath: "/x", verifyDescriptorPath: "/y" },
    ping: { kind: "fail" },
    pingBody: null,
    probed: true,
  });
  assert.equal(report.bridge.status, "dead_bridge");
});

test("composeStatusReport: projectName falls back to projectPath on empty basename", () => {
  const report = composeStatusReport({
    projectPath: "/",
    port: 22028,
    lock: null,
    classification: "gone",
    lockPath: null,
    plugins: { bridgeInstalled: false, verifyInstalled: false, bridgeDescriptorPath: null, verifyDescriptorPath: null },
    ping: null,
    pingBody: null,
    probed: false,
  });
  assert.equal(report.projectName, "/");
});
