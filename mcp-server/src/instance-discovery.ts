// P1.6 — Per-project instance discovery (TypeScript mirror).
//
// Mirror of the bridge-side FUnrealOpenMcpInstancePortResolver
// (packages/bridge/Source/UnrealOpenMcpRuntime/Public/Bridge/
// UnrealOpenMcpInstancePortResolver.h) + FUnrealOpenMcpBridgeInstanceLock
// (packages/bridge/Source/UnrealOpenMcpEditor/Private/Bridge/
// UnrealOpenMcpBridgeInstanceLock.h). The MCP server uses these to find the
// right bridge instance for a given Unreal project without any shared config:
//
//   1. UNREAL_OPEN_MCP_BRIDGE_PORT env var (override wins)
//   2. ~/.unreal-open-mcp/instances/<sha256(projectPath)>.json — the bridge's
//      instance lock / heartbeat file. We trust its `port` only when its
//      `pid` is still alive; a stale lock (crashed editor) falls through.
//   3. deterministic hash of the project path (20000 + sha256 % 10000).
//
// The hash formula MUST match the bridge exactly: first 8 bytes (16 hex chars)
// of SHA256(normalizedPath) as a big-endian 64-bit unsigned integer, mod 10000,
// + 20000. Cross-side consistency is pinned by tests in
// instance-discovery.test.ts and the bridge UnrealOpenMcpPortResolverSpec.
//
// No external deps (only node:crypto, node:fs, node:os, node:path) so the
// "no runtime deps beyond MCP SDK" rule (mcp-server/AGENTS.md) holds.
//
// Adapted from Unity Open MCP's mcp-server/src/instance-discovery.ts (copy
// fidelity), with intentional deltas documented at the bottom of this file.

import { createHash } from "node:crypto";
import { existsSync, readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

export const PORT_RANGE_START = 20000;
export const PORT_RANGE_SIZE = 10000;

/** Name advertised in the MCP `initialize` response. */
export const PORT_OVERRIDE_ENV_VAR = "UNREAL_OPEN_MCP_BRIDGE_PORT";

/** Scratch directory shared by the bridge and the MCP server. */
export const STATUS_DIR_NAME = ".unreal-open-mcp";

/** Editor state values the bridge writes into its lock / heartbeat file. */
export type InstanceState =
  | "idle"
  | "compiling"
  | "reloading"
  | "entering_playmode"
  | "playing"
  | "exiting_playmode";

/**
 * Shape of ~/.unreal-open-mcp/instances/<hash>.json (bridge
 * FUnrealOpenMcpBridgeInstanceLock). Field order mirrors the C++ BuildJson;
 * `authToken` is optional because the bridge omits it until P5.6.
 */
export interface InstanceLock {
  pid: number;
  port: number;
  /** Per-session bearer token. Optional for back-compat with locks written by
   *  bridges that predate P5.6; when absent the MCP client sends no header. */
  authToken?: string;
  projectPath: string;
  projectHash: string;
  startedAt: string;
  updatedAt: string;
  heartbeatAt: string;
  state: InstanceState;
  isPlaying: boolean;
  isCompiling: boolean;
  bridgeVersion: string;
  /** Unreal engine version (not `unityVersion`). */
  unrealVersion: string;
}

/**
 * Path normalization applied BEFORE hashing. Mirrors
 * FUnrealOpenMcpInstancePortResolver::NormalizePath in the bridge:
 *   - backslashes -> forward slashes (Windows paths hash the same cross-platform)
 *   - trailing slashes trimmed (a lone "/" is preserved)
 * No lowercasing — macOS/Linux paths are case-sensitive.
 */
export function normalizePath(projectPath: string): string {
  if (!projectPath) return "";
  let norm = projectPath.replace(/\\/g, "/");
  while (norm.length > 1 && norm.endsWith("/")) {
    norm = norm.slice(0, -1);
  }
  return norm;
}

/** Lowercase hex SHA256 of the normalized project path. */
export function projectHash(projectPath: string): string {
  return createHash("sha256").update(normalizePath(projectPath), "utf8").digest("hex");
}

/**
 * Deterministic port for a project path: 20000 + (sha256(path) % 10000).
 * Uses the first 8 bytes of the hash as a BigInt so the modulo matches the
 * C++ uint64 computation exactly.
 */
export function computePort(projectPath: string): number {
  const hash = projectHash(projectPath);
  const prefix = BigInt("0x" + hash.slice(0, 16));
  return PORT_RANGE_START + Number(prefix % BigInt(PORT_RANGE_SIZE));
}

/**
 * Base scratch directory shared by the bridge and the MCP server
 * (`~/.unreal-open-mcp`). Mirrors the C++ `SettingsDirName`. Holds the
 * `instances/` subdirectory (per-project locks).
 */
export function statusDir(): string {
  return join(homedir(), STATUS_DIR_NAME);
}

/** Directory holding one lock file per running bridge instance. */
export function instancesDir(): string {
  return join(statusDir(), "instances");
}

/** Path to this project's instance lock file. */
export function lockPath(projectPath: string): string {
  return join(instancesDir(), `${projectHash(projectPath)}.json`);
}

/**
 * Read this project's instance lock from disk. Returns null if the file is
 * missing, unreadable, or doesn't parse. Never throws — the caller falls
 * through to the deterministic hash on any failure.
 */
export function readInstanceLock(projectPath: string): InstanceLock | null {
  const path = lockPath(projectPath);
  if (!existsSync(path)) return null;
  let raw: string;
  try {
    raw = readFileSync(path, "utf8");
  } catch {
    return null;
  }
  try {
    return JSON.parse(raw) as InstanceLock;
  } catch {
    return null;
  }
}

/**
 * kill -0 equivalent. Returns true if a process with the given pid exists.
 * Wrapped in try/catch: EPERM (exists but can't be probed) → true,
 * ESRCH (no such process) → false. Mirrors the bridge's
 * FPlatformProcess::GetProcessIsAlive / FUnrealOpenMcpBridgeInstanceLock::IsPidAlive
 * logic (note: the C++ path returns false on EPERM; the TS side keeps Unity's
 * conservative true-on-EPERM so a tokenless probe never masks a live bridge).
 */
export function isPidAlive(pid: number): boolean {
  if (!pid || pid <= 0) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch (err) {
    const code = (err as NodeJS.ErrnoException).code;
    if (code === "EPERM") return true; // exists but we can't probe it
    return false; // ESRCH or anything else → treat as dead
  }
}

/**
 * Resolve the bridge port for a project, with override precedence:
 *   1. explicit envPort (already parsed + validated by the caller)
 *   2. live instance lock's port (only when its pid is alive)
 *   3. deterministic hash
 *
 * @param projectPath absolute Unreal project root
 * @param envPort     parsed UNREAL_OPEN_MCP_BRIDGE_PORT, or undefined
 */
export function resolvePort(projectPath: string, envPort?: number): number {
  if (typeof envPort === "number" && Number.isInteger(envPort) && envPort >= 1 && envPort <= 65535) {
    return envPort;
  }

  const lock = readInstanceLock(projectPath);
  if (lock && typeof lock.port === "number" && isPidAlive(lock.pid)) {
    return lock.port;
  }

  return computePort(projectPath);
}

/**
 * Resolve the bridge's per-session bearer token for a project.
 *
 * The token is only discoverable from a live instance lock (the same file we
 * read the port from). When an explicit env port override is in use there is
 * no lock file to read, so this returns undefined — in that case the MCP
 * client sends no Authorization header and the bridge must be in authMode
 * "none" for the request to succeed. Returns undefined when the lock is
 * missing, stale (dead pid), or predates the token field (P5.6 deferred —
 * the bridge omits the field today).
 *
 * @param projectPath absolute Unreal project root
 * @param envPort     parsed UNREAL_OPEN_MCP_BRIDGE_PORT, or undefined. When set,
 *                    skips the lock read (no token to discover).
 */
export function resolveAuthToken(projectPath: string, envPort?: number): string | undefined {
  if (typeof envPort === "number" && Number.isInteger(envPort) && envPort >= 1 && envPort <= 65535) {
    return undefined;
  }
  const lock = readInstanceLock(projectPath);
  if (!lock || !isPidAlive(lock.pid)) return undefined;
  const token = lock.authToken;
  return typeof token === "string" && token.length > 0 ? token : undefined;
}

// ---------------------------------------------------------------------------
// Dead-bridge detection
//
// When the bridge module itself fails to compile, the editor's startup hook
// never re-runs after a reload, so the HTTP listener never restarts and the
// heartbeat stops advancing. But the Unreal editor process is still alive
// (showing compile errors). That stale-heartbeat + live-PID signature is the
// ONLY out-of-band signal the MCP server has to distinguish "normal reload,
// will recover" from "bridge is dead, will never recover" — and it only
// exists because the bridge keeps the lock file on disk during a reload
// (releasing it only on graceful quit).

/**
 * A heartbeat is considered "stale" once it is this many milliseconds old.
 * The bridge heartbeat cadence (planned P5.7) is 0.5s, so anything older than
 * a few seconds means the heartbeat writer is no longer running. The threshold
 * is generous to absorb a slow reload without a false "dead" classification.
 */
export const HEARTBEAT_STALE_MS = 10_000;

/**
 * Coarse health classification of a bridge instance derived from its on-disk
 * lock. Used by the live-client to decide whether a /ping failure is
 * recoverable (keep waiting) or fatal (fail fast).
 *
 *   - "healthy"     — lock fresh and PID alive; bridge is (or just was) up.
 *   - "reloading"   — lock fresh + state="reloading" + PID alive; a normal
 *                     reload in flight, expected to recover.
 *   - "dead_bridge" — PID alive BUT heartbeat is stale. The editor process is
 *                     running but the bridge's heartbeat writer is gone — the
 *                     signature of a bridge module that failed to recompile.
 *                     /ping will not recover until the C++ error is fixed.
 *   - "gone"        — no lock, or PID no longer alive. No live instance.
 */
export type InstanceClassification =
  | "healthy"
  | "reloading"
  | "dead_bridge"
  | "gone";

/**
 * Age of the lock's heartbeat in milliseconds, measured against `nowMs`.
 * Returns `Infinity` when the heartbeat timestamp is missing or unparseable,
 * so a malformed lock is treated as maximally stale (and thus, if the PID is
 * alive, "dead_bridge").
 */
export function heartbeatAgeMs(
  lock: InstanceLock | null,
  nowMs: number = Date.now(),
): number {
  if (!lock || !lock.heartbeatAt) return Infinity;
  const t = Date.parse(lock.heartbeatAt);
  if (!Number.isFinite(t)) return Infinity;
  return Math.max(0, nowMs - t);
}

/**
 * Classify a bridge instance from its on-disk lock. `lock` may be null (no
 * lock file / unreadable) — treated as "gone". This is a pure function over
 * the lock snapshot + current time + PID liveness; it does no I/O of its own.
 */
export function classifyInstance(
  lock: InstanceLock | null,
  nowMs: number = Date.now(),
): InstanceClassification {
  if (!lock) return "gone";
  if (!isPidAlive(lock.pid)) return "gone";

  const age = heartbeatAgeMs(lock, nowMs);
  if (age >= HEARTBEAT_STALE_MS) return "dead_bridge";
  if (lock.state === "reloading" || lock.state === "compiling") return "reloading";
  return "healthy";
}

// Intentional deltas from Unity Open MCP (mcp-server/src/instance-discovery.ts):
//  - Home dir `~/.unreal-open-mcp` (not `~/.unity-open-mcp`).
//  - Lock field `unrealVersion` (not `unityVersion`).
//  - Skipped Unity TestRunner `hasRecentPendingTestRun` / pending TTL — Unreal
//    has no equivalent test-runner heartbeat-freeze path in P1, so the
//    dead-bridge classifier has no pending-file suppression to apply.
//  - `authToken` stays optional and is not written by the bridge until P5.6;
//    resolveAuthToken returns undefined today but is wired so P5.6 is a pure
//    additive change on the bridge side.
