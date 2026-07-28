// Compile-aware bridge readiness polling, shared by `wait-for-ready`.
//
// Adapted (copy fidelity) from Unity Open MCP's
// mcp-server/src/cli/ping-poller.ts. Polling the bridge is its own concern: it
// must respect compile state (a 503 or `compiling: true` /ping is NOT
// readiness), tolerate transient network errors during a reload, and fail fast
// on a dead-bridge signature. Pulling it out of the command implementation
// keeps the command code thin and lets the poll loop be unit-tested without
// touching the network.
//
// Intentional deltas from Unity:
//   - No MCP SDK / LiveClient dependency. The CLI is a standalone publishable
//     package with no runtime deps (ADR-007), so the poller uses the global
//     `fetch` (Node 18+) against the bridge's loopback `GET /ping` directly.
//     `fetchImpl` is injectable for tests.
//   - No `hasRecentPendingTestRun` suppression. Unreal has no equivalent
//     test-runner heartbeat-freeze path (the Unity poller keeps polling on a
//     dead_bridge signature while a TestRunner run is pending; the Unreal
//     bridge has no such signal, so a dead_bridge classification always fails
//     fast).

import {
  classifyInstance,
  readInstanceLock,
  type InstanceLock,
} from "./port-discovery.js";

/** Poll outcome. `ready` true => exit 0; false => the caller exits non-zero. */
export interface PollOutcome {
  ready: boolean;
  /** One of: ready | compiling | offline | dead_bridge | timeout. */
  status: "ready" | "compiling" | "offline" | "dead_bridge" | "timeout";
  /** Last /ping body captured (when one ever succeeded). */
  lastPing: PingBody | null;
  /** Human-friendly reason for the outcome; shown in non-JSON mode. */
  reason: string;
  /** Elapsed wall time of the poll, in ms. */
  elapsedMs: number;
  /** Number of /ping attempts made (each loop tick). */
  attempts: number;
  /** Resolved port the poller probed. Echoed so the command can print it. */
  port: number;
  /** Resolved bridge URL the poller probed (`http://127.0.0.1:<port>/ping`). */
  url: string;
}

/**
 * Subset of the bridge /ping body the poller cares about. Mirrors the bridge's
 * `GET /ping` contract (see packages/bridge — FUnrealOpenMcpBridge). Field
 * names match the mcp-server's PingBody.
 */
export interface PingBody {
  connected?: boolean;
  compiling?: boolean;
  isPlaying?: boolean;
  projectPath?: string | null;
  /** Unreal engine version (not `unityVersion`). */
  unrealVersion?: string | null;
  bridgeVersion?: string;
  mode?: string;
}

/** Default overall wait budget (aligns Unity + Unreal-MCP). */
export const DEFAULT_WAIT_TIMEOUT_MS = 120_000;
/** Default sleep between polls (Unreal-MCP uses 2s; Unity uses 1s — pick 2s). */
export const DEFAULT_POLL_INTERVAL_MS = 2_000;
/** Per-fetch timeout for a single /ping probe. */
export const PING_FETCH_TIMEOUT_MS = 5_000;

export interface PollOptions {
  /** Total budget for the wait, in ms. */
  timeoutMs: number;
  /** Sleep between polls, in ms. */
  intervalMs: number;
  /** Per-fetch timeout for a single /ping probe, in ms. */
  fetchTimeoutMs?: number;
  /** Wall clock used for deadline checks; injectable for tests. */
  now?: () => number;
  /** Sleep implementation; injectable for tests. Default: setTimeout. */
  sleep?: (ms: number) => Promise<void>;
  /**
   * Fetch implementation; injectable for tests. Default: the global `fetch`
   * (Node 18+). The poller calls `fetchImpl(url, { signal })`.
   */
  fetchImpl?: typeof fetch;
}

function defaultSleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

export interface SinglePollResult {
  /** `ready` only when connected AND not compiling. */
  status: "ready" | "compiling" | "offline" | "error";
  body: PingBody | null;
}

/**
 * Single /ping attempt translated into a poll status. Centralizes the
 * "connected AND not compiling" readiness rule so every caller agrees on it.
 *
 * - network error / non-2xx (except 503) -> `offline` (the bridge is not
 *   listening yet — keep waiting).
 * - HTTP 503 (compile in progress) -> `compiling` (keep waiting; the listener
 *   is up but the editor is mid-compile).
 * - 2xx with `compiling: true` -> `compiling`.
 * - 2xx with `connected: false` -> `offline` (bridge up but not yet wired to
 *   the editor game thread).
 * - 2xx otherwise -> `ready`.
 *
 * @param url        Full `http://127.0.0.1:<port>/ping` URL.
 * @param fetchImpl  Fetch implementation (injectable; default global fetch).
 * @param timeoutMs  Per-fetch AbortSignal timeout, in ms.
 */
export async function singlePing(
  url: string,
  fetchImpl: typeof fetch = fetch,
  timeoutMs: number = PING_FETCH_TIMEOUT_MS,
): Promise<SinglePollResult> {
  // AbortController is available on Node 18+. The `signal` is the only cross-
  // runtime way to bound a single fetch; the bridge can take a few seconds to
  // answer during a reload.
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const res = await fetchImpl(url, { signal: ctrl.signal });
    // 503 is the bridge's "compile in progress" status — the listener is up,
    // so this is NOT readiness but it IS recoverable.
    if (res.status === 503) {
      return { status: "compiling", body: null };
    }
    if (!res.ok) {
      // Any other non-2xx (404, 500, ...) — treat as offline; the poller keeps
      // waiting. A 404 specifically means the bridge is up but `/ping` is not
      // routed (wrong binary), which the dead-bridge classifier will surface
      // separately if the PID is alive but the heartbeat is stale.
      return { status: "offline", body: null };
    }
    let body: PingBody | null = null;
    try {
      const text = await res.text();
      body = text ? (JSON.parse(text) as PingBody) : null;
    } catch {
      body = null;
    }
    if (body?.compiling === true) {
      return { status: "compiling", body };
    }
    if (body?.connected === false) {
      return { status: "offline", body };
    }
    return { status: "ready", body };
  } catch {
    // Network error (ECONNREFUSED while the editor is still booting) or an
    // AbortError from the per-fetch timeout — both are transient during a
    // reload, so classify as `error` (kept distinct from `offline` so the
    // poller can attribute its timeout reason accurately, but treated the same
    // way: keep waiting unless the dead-bridge signature fires).
    return { status: "error", body: null };
  } finally {
    clearTimeout(timer);
  }
}

/**
 * Poll the bridge until it is ready (connected, not compiling) or the deadline
 * passes. A 503 from /ping and `compiling: true` both keep the wait alive —
 * "ready" means usable, not just listening. The poll fails fast on a
 * dead-bridge signature (live PID, stale heartbeat) because /ping will never
 * recover in that state.
 *
 * @param url           Full `http://127.0.0.1:<port>/ping` URL.
 * @param port          Resolved port (echoed in the outcome).
 * @param projectPath   Absolute project root, used to read the instance lock
 *                      for dead-bridge classification. Optional; when absent
 *                      the dead-bridge shortcut is skipped.
 * @param lockSnapshot  An already-read instance lock, when the caller wants the
 *                      port + token + classification to share a single read
 *                      (the bridge rewrites the lock on every start). When
 *                      undefined, the poller reads the lock on each dead-bridge
 *                      probe. Optional.
 * @param opts          Timing + clock + fetch injection.
 */
export async function pollUntilReady(
  url: string,
  port: number,
  projectPath: string | undefined,
  lockSnapshot: InstanceLock | null | undefined,
  opts: PollOptions,
): Promise<PollOutcome> {
  const now = opts.now ?? Date.now;
  const sleep = opts.sleep ?? defaultSleep;
  const fetchImpl = opts.fetchImpl ?? fetch;
  const fetchTimeoutMs = opts.fetchTimeoutMs ?? PING_FETCH_TIMEOUT_MS;
  const start = now();
  const deadline = start + opts.timeoutMs;

  let lastPing: PingBody | null = null;
  let sawCompiling = false;
  let sawOffline = false;
  let attempts = 0;

  while (true) {
    const t = now();
    if (t >= deadline) {
      return {
        ready: false,
        status: "timeout",
        lastPing,
        elapsedMs: t - start,
        attempts,
        port,
        url,
        reason: formatTimeoutReason(opts.timeoutMs, sawCompiling, sawOffline),
      };
    }

    attempts += 1;
    const result = await singlePing(url, fetchImpl, fetchTimeoutMs);

    if (result.body) lastPing = result.body;
    if (result.status === "compiling") sawCompiling = true;
    if (result.status === "offline" || result.status === "error") {
      sawOffline = true;
    }

    if (result.status === "ready") {
      return {
        ready: true,
        status: "ready",
        lastPing: result.body ?? lastPing,
        elapsedMs: now() - start,
        attempts,
        port,
        url,
        reason: "Bridge is ready (connected, idle).",
      };
    }

    // Dead-bridge signature: the editor process is alive but the bridge's
    // startup hook never re-ran after a compile failure. /ping will not
    // recover until the C++ error is fixed, so waiting is pointless.
    if (projectPath) {
      let classification;
      try {
        const lock = lockSnapshot !== undefined ? lockSnapshot : readInstanceLock(projectPath);
        classification = classifyInstance(lock);
      } catch {
        classification = null;
      }
      if (classification === "dead_bridge") {
        return {
          ready: false,
          status: "dead_bridge",
          lastPing,
          elapsedMs: now() - start,
          attempts,
          port,
          url,
          reason:
            "Bridge assembly failed to recompile — the Unreal Editor is in a " +
            "bad state (showing compile errors). Open the editor, fix the C++ " +
            "errors, then re-run wait-for-ready.",
        };
      }
    }

    await sleep(Math.min(opts.intervalMs, Math.max(0, deadline - now())));
  }
}

function formatTimeoutReason(
  timeoutMs: number,
  sawCompiling: boolean,
  sawOffline: boolean,
): string {
  const secs = Math.round(timeoutMs / 1000);
  if (sawCompiling && !sawOffline) {
    return `Bridge was still compiling after ${secs}s.`;
  }
  if (sawOffline && !sawCompiling) {
    return `Bridge never became reachable within ${secs}s.`;
  }
  return `Bridge did not become ready within ${secs}s.`;
}
