import test from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, writeFileSync, existsSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { bridgeStatus } from "./bridge-status.js";
import { ALL_TOOLS } from "./index.js";
import {
  deriveBridgeStatus,
  bridgeStatusRecoveryHint,
  bridgeStatusNextStep,
  summarizeInstanceLock,
  type PingProbe,
} from "./bridge-status-helpers.js";
import {
  projectHash,
  instancesDir,
  type InstanceLock,
} from "../instance-discovery.js";
import { handleLocalTool, setBoundProjectPath } from "../index.js";
import type { CallToolResult } from "@modelcontextprotocol/sdk/types.js";

// ---------------------------------------------------------------------------
// Tool definition / registration
// ---------------------------------------------------------------------------

test("bridge_status tool is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(bridgeStatus.name, "unreal_open_mcp_bridge_status");
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_bridge_status"),
    "must be in ALL_TOOLS",
  );
});

test("bridge_status schema is an empty-args object (no inputs)", () => {
  const schema = bridgeStatus.inputSchema as {
    type: string;
    properties: Record<string, unknown>;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  assert.deepEqual(schema.properties, {});
  assert.equal(schema.additionalProperties, false);
});

test("bridge_status description documents the status vocabulary, route, and recovery hint", () => {
  const desc = bridgeStatus.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  // The five status tokens.
  for (const token of [
    "running",
    "compiling",
    "stopped",
    "unreachable",
    "dead_bridge",
  ]) {
    assert.ok(desc.includes(token), `description must mention ${token}`);
  }
  // Route is local (no bridge POST) + read-only / gate-free.
  assert.match(desc, /local/i);
  assert.match(desc, /read-only/i);
  assert.match(desc, /gate-free/i);
  // The recovery hint names read_compile_errors (the offline editor-log reader
  // shipped in P8.7 — the one channel that works when the bridge module itself
  // failed to compile).
  assert.match(desc, /unreal_open_mcp_read_compile_errors/);
  // classification + recoveryHint + nextStep fields surfaced.
  assert.match(desc, /classification/);
  assert.match(desc, /recoveryHint/);
  assert.match(desc, /nextStep/);
});

// ---------------------------------------------------------------------------
// Pure status mapper — the authoritative matrix
// ---------------------------------------------------------------------------

const pingOk = (overrides: Partial<{
  connected: boolean;
  compiling: boolean;
  isPlaying: boolean;
}> = {}): PingProbe => ({
  kind: "ok",
  connected: overrides.connected ?? true,
  compiling: overrides.compiling ?? false,
  isPlaying: overrides.isPlaying ?? false,
});

const pingFail: PingProbe = { kind: "fail" };

test("deriveBridgeStatus: healthy + ping connected + not compiling → running", () => {
  assert.equal(
    deriveBridgeStatus({ classification: "healthy", ping: pingOk() }),
    "running",
  );
});

test("deriveBridgeStatus: reloading + ping connected + compiling → compiling", () => {
  assert.equal(
    deriveBridgeStatus({
      classification: "reloading",
      ping: pingOk({ connected: true, compiling: true }),
    }),
    "compiling",
  );
});

test("deriveBridgeStatus: healthy + ping connected + compiling → compiling", () => {
  // The lock says healthy but the live ping reports a compile in progress —
  // the live signal wins so an agent waits.
  assert.equal(
    deriveBridgeStatus({
      classification: "healthy",
      ping: pingOk({ connected: true, compiling: true }),
    }),
    "compiling",
  );
});

test("deriveBridgeStatus: reloading + ping fail → unreachable", () => {
  assert.equal(
    deriveBridgeStatus({ classification: "reloading", ping: pingFail }),
    "unreachable",
  );
});

test("deriveBridgeStatus: reloading + null probe → unreachable", () => {
  assert.equal(
    deriveBridgeStatus({ classification: "reloading", ping: null }),
    "unreachable",
  );
});

test("deriveBridgeStatus: dead_bridge + any ping → dead_bridge", () => {
  // The lock's stale-heartbeat + live-PID signature is authoritative: the
  // bridge assembly is dead regardless of whether /ping happened to land.
  assert.equal(
    deriveBridgeStatus({ classification: "dead_bridge", ping: pingOk() }),
    "dead_bridge",
  );
  assert.equal(
    deriveBridgeStatus({ classification: "dead_bridge", ping: pingFail }),
    "dead_bridge",
  );
  assert.equal(
    deriveBridgeStatus({ classification: "dead_bridge", ping: null }),
    "dead_bridge",
  );
});

test("deriveBridgeStatus: gone + ping fail → stopped", () => {
  assert.equal(
    deriveBridgeStatus({ classification: "gone", ping: pingFail }),
    "stopped",
  );
});

test("deriveBridgeStatus: gone + null probe → stopped", () => {
  assert.equal(
    deriveBridgeStatus({ classification: "gone", ping: null }),
    "stopped",
  );
});

test("deriveBridgeStatus: healthy + ping fail → stopped", () => {
  // No cold-Safe-Mode process scan (Unity delta): a failed ping with a healthy
  // lock stays stopped rather than narrowing to dead_bridge.
  assert.equal(
    deriveBridgeStatus({ classification: "healthy", ping: pingFail }),
    "stopped",
  );
});

test("deriveBridgeStatus: ping connected:false is treated as not-running", () => {
  // The bridge answered but reported connected:false — not a healthy listener.
  assert.equal(
    deriveBridgeStatus({
      classification: "healthy",
      ping: pingOk({ connected: false, compiling: false }),
    }),
    "stopped",
  );
});

test("deriveBridgeStatus: dead_bridge precedence beats a compiling ping", () => {
  assert.equal(
    deriveBridgeStatus({
      classification: "dead_bridge",
      ping: pingOk({ connected: true, compiling: true }),
    }),
    "dead_bridge",
  );
});

// ---------------------------------------------------------------------------
// recoveryHint policy — non-null only for dead_bridge, and points at the
// interim console surface (dedicated offline reader is deferred)
// ---------------------------------------------------------------------------

test("recoveryHint is null for running / compiling / stopped / unreachable", () => {
  for (const status of [
    "running",
    "compiling",
    "stopped",
    "unreachable",
  ] as const) {
    assert.equal(bridgeStatusRecoveryHint(status), null, `${status} → null`);
  }
});

test("recoveryHint for dead_bridge points at read_compile_errors", () => {
  const hint = bridgeStatusRecoveryHint("dead_bridge");
  assert.ok(hint, "dead_bridge must carry a hint");
  assert.equal(hint.tool, "unreal_open_mcp_read_compile_errors");
  assert.ok(hint.reason.length > 0, "reason must be non-empty");
});

// ---------------------------------------------------------------------------
// nextStep — present + status-appropriate for every token
// ---------------------------------------------------------------------------

test("nextStep is a non-empty string for every status token", () => {
  for (const status of [
    "running",
    "compiling",
    "stopped",
    "unreachable",
    "dead_bridge",
  ] as const) {
    const step = bridgeStatusNextStep(status);
    assert.ok(typeof step === "string" && step.length > 0, `${status} step`);
  }
});

test("nextStep for dead_bridge references read_compile_errors", () => {
  assert.match(
    bridgeStatusNextStep("dead_bridge"),
    /unreal_open_mcp_read_compile_errors/,
  );
});

test("nextStep for unreachable mentions retry", () => {
  assert.match(bridgeStatusNextStep("unreachable"), /retry/i);
});

// ---------------------------------------------------------------------------
// summarizeInstanceLock — null-safe lock redaction
// ---------------------------------------------------------------------------

test("summarizeInstanceLock returns null for no lock", () => {
  assert.equal(summarizeInstanceLock(null), null);
});

test("summarizeInstanceLock redacts the lock to the operator-facing fields", () => {
  const lock: InstanceLock = {
    pid: 4242,
    port: 22028,
    authToken: "deadbeef".repeat(8),
    projectPath: "/proj",
    projectHash: projectHash("/proj"),
    startedAt: "2026-07-12T00:00:00.000Z",
    updatedAt: "2026-07-12T00:00:01.000Z",
    heartbeatAt: "2026-07-12T00:00:01.000Z",
    state: "idle",
    isPlaying: false,
    isCompiling: false,
    bridgeVersion: "0.0.1",
    unrealVersion: "5.8.0",
  };
  const summary = summarizeInstanceLock(lock);
  assert.deepEqual(summary, {
    pid: 4242,
    port: 22028,
    state: "idle",
    isCompiling: false,
    isPlaying: false,
    heartbeatAt: "2026-07-12T00:00:01.000Z",
    bridgeVersion: "0.0.1",
    unrealVersion: "5.8.0",
  });
});

// ---------------------------------------------------------------------------
// Local-route integration — handleLocalTool composes lock + router ping.
// Drives the in-process handler directly with stub locks + a stub router so the
// full status-decision tree (and the isError:false contract) is pinned without
// any network.
// ---------------------------------------------------------------------------

/**
 * A stub live router whose /ping returns the given CallToolResult. Used to feed
 * a controlled ping outcome into the bridge_status handler.
 */
function makeStubRouter(pingResult: CallToolResult): {
  router: { route: (name: string, args: Record<string, unknown>) => Promise<CallToolResult> };
  calls: { tool: string; args: Record<string, unknown> }[];
} {
  const calls: { tool: string; args: Record<string, unknown> }[] = [];
  return {
    calls,
    router: {
      async route(name: string, args: Record<string, unknown>) {
        calls.push({ tool: name, args });
        assert.equal(name, "unreal_open_mcp_ping");
        return pingResult;
      },
    },
  };
}

function pingResultOk(body: Record<string, unknown>): CallToolResult {
  return {
    content: [{ type: "text", text: JSON.stringify(body) }],
    isError: false,
  };
}

function pingResultFail(): CallToolResult {
  return {
    content: [
      { type: "text", text: JSON.stringify({ error: { code: "bridge_offline" } }) },
    ],
    isError: true,
  };
}

const RUNNER_PID = process.pid; // guaranteed alive — it's the test process

/**
 * Redirect `homedir()` at a temp dir for the lock-planting tests, mirroring the
 * instance-discovery test sandbox. readInstanceLock resolves homedir() fresh on
 * every call, so this is safe without module reloads. Restored in `finally`.
 */
interface Sandbox {
  dir: string;
  prevHome: string | undefined;
  prevUserProfile: string | undefined;
}

function makeSandbox(): Sandbox {
  const dir = mkdtempSync(join(tmpdir(), "uomcp-bridge-status-"));
  return {
    dir,
    prevHome: process.env.HOME,
    prevUserProfile: process.env.USERPROFILE,
  };
}

function applySandbox(s: Sandbox): void {
  process.env.HOME = s.dir;
  process.env.USERPROFILE = s.dir;
}

function cleanupSandbox(s: Sandbox): void {
  if (s.prevHome === undefined) delete process.env.HOME;
  else process.env.HOME = s.prevHome;
  if (s.prevUserProfile === undefined) delete process.env.USERPROFILE;
  else process.env.USERPROFILE = s.prevUserProfile;
  try {
    rmSync(s.dir, { recursive: true, force: true });
  } catch {
    // best-effort
  }
}

interface PlantOpts {
  port?: number;
  pid?: number;
  heartbeatAgeMs?: number;
  state?: InstanceLock["state"];
  isCompiling?: boolean;
  authToken?: string;
}

/** Plant a lock for `projectPath` into the sandbox's instances dir. */
function plantProjectLock(
  sandbox: Sandbox,
  projectPath: string,
  opts: PlantOpts = {},
): void {
  applySandbox(sandbox);
  const hash = projectHash(projectPath);
  const heartbeat = new Date(
    Date.now() - (opts.heartbeatAgeMs ?? 1_000),
  ).toISOString();
  const payload: InstanceLock = {
    pid: opts.pid ?? RUNNER_PID,
    port: opts.port ?? 22028,
    projectPath,
    projectHash: hash,
    startedAt: heartbeat,
    updatedAt: heartbeat,
    heartbeatAt: heartbeat,
    state: opts.state ?? "idle",
    isPlaying: false,
    isCompiling: opts.isCompiling ?? false,
    bridgeVersion: "0.0.1",
    unrealVersion: "5.8.0",
    ...(opts.authToken !== undefined ? { authToken: opts.authToken } : {}),
  };
  const dir = instancesDir();
  if (!existsSync(dir)) mkdirSync(dir, { recursive: true });
  writeFileSync(join(dir, `${hash}.json`), JSON.stringify(payload));
}

const TMP_PROJECT = "/tmp/UomcpBridgeStatusTestProj";

test("bridge_status returns running when ping is connected + idle (no lock → classification gone)", async () => {
  const sandbox = makeSandbox();
  setBoundProjectPath(TMP_PROJECT);
  try {
    const { router, calls } = makeStubRouter(
      pingResultOk({ connected: true, compiling: false, isPlaying: false }),
    );
    const result = await handleLocalTool(
      "unreal_open_mcp_bridge_status",
      {},
      router,
    );
    assert.ok(result, "must resolve as a local tool");
    assert.equal(result.isError, false);
    const body = JSON.parse(
      (result.content[0] as { text: string }).text,
    ) as Record<string, unknown>;
    assert.equal(body.status, "running");
    assert.equal(body.ready, true);
    assert.equal(body.classification, "gone");
    assert.equal(body.recoveryHint, null);
    assert.equal((body.ping as { reachable: boolean }).reachable, true);
    // Exactly one ping probe fired.
    assert.deepEqual(
      calls.map((c) => c.tool),
      ["unreal_open_mcp_ping"],
    );
  } finally {
    setBoundProjectPath(null);
    cleanupSandbox(sandbox);
  }
});

test("bridge_status returns compiling when ping reports compiling=true", async () => {
  const sandbox = makeSandbox();
  setBoundProjectPath(TMP_PROJECT);
  try {
    const { router } = makeStubRouter(
      pingResultOk({ connected: true, compiling: true }),
    );
    const result = await handleLocalTool(
      "unreal_open_mcp_bridge_status",
      {},
      router,
    );
    const body = JSON.parse(
      (result!.content[0] as { text: string }).text,
    ) as Record<string, unknown>;
    assert.equal(body.status, "compiling");
    assert.equal(body.ready, false);
  } finally {
    setBoundProjectPath(null);
    cleanupSandbox(sandbox);
  }
});

test("bridge_status returns stopped when bridge is offline + no lock", async () => {
  const sandbox = makeSandbox();
  setBoundProjectPath(TMP_PROJECT);
  try {
    const { router } = makeStubRouter(pingResultFail());
    const result = await handleLocalTool(
      "unreal_open_mcp_bridge_status",
      {},
      router,
    );
    assert.equal(result!.isError, false, "stopped is a successful status read");
    const body = JSON.parse(
      (result!.content[0] as { text: string }).text,
    ) as Record<string, unknown>;
    assert.equal(body.status, "stopped");
    assert.equal(body.ready, false);
    assert.equal(body.classification, "gone");
    assert.equal(body.recoveryHint, null);
    assert.equal((body.ping as { reachable: boolean }).reachable, false);
    assert.equal((body.instance as { lock: unknown }).lock, null);
  } finally {
    setBoundProjectPath(null);
    cleanupSandbox(sandbox);
  }
});

test("bridge_status returns dead_bridge when the lock classifies dead + ping fails", async () => {
  const sandbox = makeSandbox();
  setBoundProjectPath(TMP_PROJECT);
  // Plant a live-pid lock with a stale heartbeat → classifyInstance dead_bridge.
  plantProjectLock(sandbox, TMP_PROJECT, {
    heartbeatAgeMs: 60_000,
    state: "reloading",
  });
  try {
    const { router } = makeStubRouter(pingResultFail());
    const result = await handleLocalTool(
      "unreal_open_mcp_bridge_status",
      {},
      router,
    );
    const body = JSON.parse(
      (result!.content[0] as { text: string }).text,
    ) as Record<string, unknown>;
    assert.equal(body.status, "dead_bridge");
    assert.equal(body.classification, "dead_bridge");
    assert.equal(body.ready, false);
    const hint = body.recoveryHint as { tool: string; reason: string };
    assert.equal(hint.tool, "unreal_open_mcp_read_compile_errors");
    assert.match(body.nextStep as string, /unreal_open_mcp_read_compile_errors/);
    // instance.lock is populated from the planted lock.
    const lock = (body.instance as { lock: { pid: number } }).lock;
    assert.equal(lock.pid, RUNNER_PID);
  } finally {
    setBoundProjectPath(null);
    cleanupSandbox(sandbox);
  }
});

test("bridge_status returns unreachable when lock is reloading + ping fails", async () => {
  const sandbox = makeSandbox();
  setBoundProjectPath(TMP_PROJECT);
  plantProjectLock(sandbox, TMP_PROJECT, {
    heartbeatAgeMs: 1_000,
    state: "reloading",
  });
  try {
    const { router } = makeStubRouter(pingResultFail());
    const result = await handleLocalTool(
      "unreal_open_mcp_bridge_status",
      {},
      router,
    );
    const body = JSON.parse(
      (result!.content[0] as { text: string }).text,
    ) as Record<string, unknown>;
    assert.equal(body.status, "unreachable");
    assert.equal(body.ready, false);
    assert.equal(body.classification, "reloading");
    assert.equal(body.recoveryHint, null);
    assert.match(body.nextStep as string, /retry/i);
  } finally {
    setBoundProjectPath(null);
    cleanupSandbox(sandbox);
  }
});

test("bridge_status treats a null router as a failed ping (stopped with no lock)", async () => {
  const sandbox = makeSandbox();
  setBoundProjectPath(TMP_PROJECT);
  try {
    const result = await handleLocalTool(
      "unreal_open_mcp_bridge_status",
      {},
      null,
    );
    assert.equal(result!.isError, false);
    const body = JSON.parse(
      (result!.content[0] as { text: string }).text,
    ) as Record<string, unknown>;
    // No lock + no router → stopped (the probe is treated as failed).
    assert.equal(body.status, "stopped");
    assert.equal((body.ping as { reachable: boolean }).reachable, false);
  } finally {
    setBoundProjectPath(null);
    cleanupSandbox(sandbox);
  }
});

test("bridge_status never returns isError:true across every classification branch", async () => {
  // A dead / stopped bridge is a successful status read — pin the contract by
  // driving several branches with a failing router; none may error.
  const sandbox = makeSandbox();
  setBoundProjectPath(TMP_PROJECT);
  try {
    const { router } = makeStubRouter(pingResultFail());
    for (const state of ["healthy", "reloading", "dead_bridge", "gone"] as const) {
      const result = await handleLocalTool(
        "unreal_open_mcp_bridge_status",
        {},
        router,
      );
      assert.equal(result!.isError, false, `${state} branch must not error`);
    }
  } finally {
    setBoundProjectPath(null);
    cleanupSandbox(sandbox);
  }
});

test("handleLocalTool returns null for a non-local tool", async () => {
  // A live-route tool name is not a local-route tool → null (caller routes live).
  const result = await handleLocalTool("unreal_open_mcp_ping", {}, null);
  assert.equal(result, null);
});
