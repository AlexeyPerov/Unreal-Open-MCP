// Tests for `ping-poller.ts` — the compile-aware bridge readiness poller.
// Adapted from Unity Open MCP's mcp-server/src/cli/ping-poller.test.ts shape,
// with the CLI's raw-fetch + no-LiveClient deltas. The poll loop is fully
// driven by injected `fetchImpl` / `sleep` / `now`, so these tests never touch
// the network or real time.
//
// Built + run via the package test config (see package.json `test`).

import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";

import {
  singlePing,
  pollUntilReady,
  DEFAULT_WAIT_TIMEOUT_MS,
  DEFAULT_POLL_INTERVAL_MS,
  type PingBody,
  type SinglePollResult,
  type PollOutcome,
} from "./ping-poller.js";
import { lockPath } from "./port-discovery.js";

// ---------------------------------------------------------------------------
// Test fakes
// ---------------------------------------------------------------------------

/**
 * A scripted fetch: returns each response in `script` in order, then loops the
 * last one. Each entry is either a `{ status, body }` (the HTTP response) or a
 * `{ throw }` (a network error). Captures the URL each call received.
 */
type ScriptedResponse =
  | { status: number; body?: PingBody | null }
  | { throw: string };

function makeScriptedFetch(script: ScriptedResponse[]): {
  fetch: typeof fetch;
  calls: string[];
} {
  const calls: string[] = [];
  let i = 0;
  const fetchImpl = (async (url: string | URL | Request, _init?: RequestInit) => {
    calls.push(String(url));
    const entry = script[Math.min(i, script.length - 1)];
    i++;
    if ("throw" in entry) {
      throw new Error(entry.throw);
    }
    const status = entry.status;
    const bodyText =
      entry.body === null || entry.body === undefined
        ? ""
        : JSON.stringify(entry.body);
    return new Response(bodyText, { status });
  }) as typeof fetch;
  return { fetch: fetchImpl, calls };
}

/** Deterministic clock: advances by `stepMs` on every read. */
function makeSteppingClock(stepMs: number, start = 0): { now: () => number; t: number } {
  const state = { t: start };
  return {
    now: () => {
      const v = state.t;
      state.t += stepMs;
      return v;
    },
    t: 0,
  };
}

// ---------------------------------------------------------------------------
// singlePing
// ---------------------------------------------------------------------------

test("singlePing: 2xx with connected body => ready", async () => {
  const { fetch } = makeScriptedFetch([
    { status: 200, body: { connected: true, compiling: false } },
  ]);
  const r: SinglePollResult = await singlePing("http://127.0.0.1:22028/ping", fetch);
  assert.equal(r.status, "ready");
  assert.deepEqual(r.body, { connected: true, compiling: false });
});

test("singlePing: 2xx with compiling:true => compiling", async () => {
  const { fetch } = makeScriptedFetch([
    { status: 200, body: { connected: true, compiling: true } },
  ]);
  const r = await singlePing("http://127.0.0.1:22028/ping", fetch);
  assert.equal(r.status, "compiling");
});

test("singlePing: 2xx with connected:false => offline", async () => {
  const { fetch } = makeScriptedFetch([
    { status: 200, body: { connected: false, compiling: false } },
  ]);
  const r = await singlePing("http://127.0.0.1:22028/ping", fetch);
  assert.equal(r.status, "offline");
});

test("singlePing: HTTP 503 => compiling (recoverable)", async () => {
  const { fetch } = makeScriptedFetch([{ status: 503 }]);
  const r = await singlePing("http://127.0.0.1:22028/ping", fetch);
  assert.equal(r.status, "compiling");
  assert.equal(r.body, null);
});

test("singlePing: HTTP 404 => offline", async () => {
  const { fetch } = makeScriptedFetch([{ status: 404 }]);
  const r = await singlePing("http://127.0.0.1:22028/ping", fetch);
  assert.equal(r.status, "offline");
});

test("singlePing: network error (fetch throws) => error", async () => {
  const { fetch } = makeScriptedFetch([{ throw: "ECONNREFUSED" }]);
  const r = await singlePing("http://127.0.0.1:22028/ping", fetch);
  assert.equal(r.status, "error");
  assert.equal(r.body, null);
});

test("singlePing: 2xx with unparseable body => ready (body null, defaults hold)", async () => {
  // A 2xx with no compiling/connected fields is treated as ready — the bridge
  // is up and did not report a non-ready state.
  const fetchImpl = (async () => new Response("not-json", { status: 200 })) as typeof fetch;
  const r = await singlePing("http://127.0.0.1:22028/ping", fetchImpl);
  assert.equal(r.status, "ready");
  assert.equal(r.body, null);
});

// ---------------------------------------------------------------------------
// pollUntilReady — happy paths
// ---------------------------------------------------------------------------

test("pollUntilReady: ready on the first probe => exit ready, 1 attempt", async () => {
  const { fetch } = makeScriptedFetch([
    { status: 200, body: { connected: true, compiling: false } },
  ]);
  const clock = makeSteppingClock(0, 1000);
  const sleeps: number[] = [];
  const outcome: PollOutcome = await pollUntilReady(
    "http://127.0.0.1:22028/ping",
    22028,
    undefined,
    undefined,
    {
      timeoutMs: DEFAULT_WAIT_TIMEOUT_MS,
      intervalMs: DEFAULT_POLL_INTERVAL_MS,
      now: clock.now,
      sleep: async (ms) => {
        sleeps.push(ms);
      },
      fetchImpl: fetch,
    },
  );
  assert.equal(outcome.ready, true);
  assert.equal(outcome.status, "ready");
  assert.equal(outcome.attempts, 1);
  // No sleep on a ready-first-probe.
  assert.deepEqual(sleeps, []);
});

test("pollUntilReady: offline then ready => 2 attempts, 1 sleep", async () => {
  const { fetch } = makeScriptedFetch([
    { throw: "ECONNREFUSED" },
    { status: 200, body: { connected: true, compiling: false } },
  ]);
  const clock = makeSteppingClock(0, 1000);
  const sleeps: number[] = [];
  const outcome = await pollUntilReady(
    "http://127.0.0.1:22028/ping",
    22028,
    undefined,
    undefined,
    {
      timeoutMs: 10_000,
      intervalMs: 2_000,
      now: clock.now,
      sleep: async (ms) => {
        sleeps.push(ms);
      },
      fetchImpl: fetch,
    },
  );
  assert.equal(outcome.ready, true);
  assert.equal(outcome.attempts, 2);
  assert.deepEqual(sleeps, [2_000]);
});

test("pollUntilReady: compiling (503) then ready => 2 attempts, reason tracks compiling", async () => {
  const { fetch } = makeScriptedFetch([
    { status: 503 },
    { status: 200, body: { connected: true, compiling: false } },
  ]);
  const outcome = await pollUntilReady(
    "http://127.0.0.1:22028/ping",
    22028,
    undefined,
    undefined,
    {
      timeoutMs: 10_000,
      intervalMs: 1_000,
      now: () => 0, // frozen clock: deadline never hits
      sleep: async () => {},
      fetchImpl: fetch,
    },
  );
  assert.equal(outcome.ready, true);
  assert.equal(outcome.attempts, 2);
});

// ---------------------------------------------------------------------------
// pollUntilReady — timeout
// ---------------------------------------------------------------------------

test("pollUntilReady: never reachable => timeout, non-ready", async () => {
  const { fetch } = makeScriptedFetch([{ throw: "ECONNREFUSED" }]);
  // A clock that lets the loop run a few iterations (start + several probes at
  // t=0), then jumps past the 5s deadline so the next loop-top check fires.
  // Reads: start, loop-check, (probe), sleep-remaining, loop-check, ... We keep
  // t=0 for the first several reads so probes run, then advance past deadline.
  let call = 0;
  const now = () => {
    call++;
    // Reads 1-6 stay at 0 (start + a few probes); read 7+ is past the deadline.
    return call < 7 ? 0 : 10_000;
  };
  const outcome = await pollUntilReady(
    "http://127.0.0.1:22028/ping",
    22028,
    undefined,
    undefined,
    {
      timeoutMs: 5_000,
      intervalMs: 1_000,
      now,
      sleep: async () => {},
      fetchImpl: fetch,
    },
  );
  assert.equal(outcome.ready, false);
  assert.equal(outcome.status, "timeout");
  assert.match(outcome.reason, /never became reachable/);
});

test("pollUntilReady: stuck compiling => timeout with compiling reason", async () => {
  const { fetch } = makeScriptedFetch([{ status: 503 }]);
  let call = 0;
  const now = () => {
    call++;
    return call < 7 ? 0 : 10_000;
  };
  const outcome = await pollUntilReady(
    "http://127.0.0.1:22028/ping",
    22028,
    undefined,
    undefined,
    {
      timeoutMs: 5_000,
      intervalMs: 1_000,
      now,
      sleep: async () => {},
      fetchImpl: fetch,
    },
  );
  assert.equal(outcome.status, "timeout");
  assert.match(outcome.reason, /still compiling/);
});

// ---------------------------------------------------------------------------
// pollUntilReady — dead-bridge fail-fast
// ---------------------------------------------------------------------------

test("pollUntilReady: dead-bridge signature => fail fast (dead_bridge)", async () => {
  // Offline probe + a lock whose PID is alive but heartbeat is stale. The
  // poller must fail fast instead of spinning to timeout.
  const projectPath = "/Users/foo/DeadBridgeGame";
  const stale = Date.now() - 15_000; // > HEARTBEAT_STALE_MS (10s)
  const lock = {
    pid: process.pid, // alive
    port: 22028,
    projectPath,
    heartbeatAt: new Date(stale).toISOString(),
    state: "idle",
  };
  // Write a real lock file so the poller's readInstanceLock finds it (we pass
  // undefined for lockSnapshot to exercise the read path).
  const p = lockPath(projectPath);
  fs.mkdirSync(path.dirname(p), { recursive: true });
  fs.writeFileSync(p, JSON.stringify(lock), "utf8");
  try {
    const { fetch } = makeScriptedFetch([{ throw: "ECONNREFUSED" }]);
    const outcome = await pollUntilReady(
      "http://127.0.0.1:22028/ping",
      22028,
      projectPath,
      undefined, // force the poller to read the lock itself
      {
        timeoutMs: 5_000,
        intervalMs: 1_000,
        now: () => 0,
        sleep: async () => {},
        fetchImpl: fetch,
      },
    );
    assert.equal(outcome.ready, false);
    assert.equal(outcome.status, "dead_bridge");
    assert.match(outcome.reason, /failed to recompile/i);
  } finally {
    try {
      fs.unlinkSync(p);
    } catch {
      // best-effort
    }
  }
});

test("pollUntilReady: passed lock snapshot is used (no re-read)", async () => {
  // A stale-heartbeat lock snapshot passed directly must trigger dead_bridge
  // even when no lock file exists on disk.
  const projectPath = "/Users/foo/SnapshotDeadBridgeGame";
  const stale = Date.now() - 15_000;
  const lock = {
    pid: process.pid,
    port: 22028,
    projectPath,
    heartbeatAt: new Date(stale).toISOString(),
    state: "idle",
  };
  const { fetch } = makeScriptedFetch([{ throw: "ECONNREFUSED" }]);
  const outcome = await pollUntilReady(
    "http://127.0.0.1:22028/ping",
    22028,
    projectPath,
    lock as any,
    {
      timeoutMs: 5_000,
      intervalMs: 1_000,
      now: () => 0,
      sleep: async () => {},
      fetchImpl: fetch,
    },
  );
  assert.equal(outcome.status, "dead_bridge");
});

test("pollUntilReady: healthy lock + offline probe keeps waiting (no false dead_bridge)", async () => {
  // A FRESH heartbeat + live PID is `healthy`, not `dead_bridge` — the poller
  // must keep waiting through transient offline probes.
  const projectPath = "/Users/foo/HealthyBridgeGame";
  const lock = {
    pid: process.pid,
    port: 22028,
    projectPath,
    heartbeatAt: new Date().toISOString(), // fresh
    state: "idle",
  };
  const { fetch } = makeScriptedFetch([
    { throw: "ECONNREFUSED" },
    { status: 200, body: { connected: true, compiling: false } },
  ]);
  const outcome = await pollUntilReady(
    "http://127.0.0.1:22028/ping",
    22028,
    projectPath,
    lock as any,
    {
      timeoutMs: 10_000,
      intervalMs: 1_000,
      now: () => 0, // frozen: deadline never hits
      sleep: async () => {},
      fetchImpl: fetch,
    },
  );
  assert.equal(outcome.ready, true);
});
