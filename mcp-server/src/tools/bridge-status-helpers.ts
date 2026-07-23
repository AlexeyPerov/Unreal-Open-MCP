// P5.7 — pure status mapper for `unreal_open_mcp_bridge_status`.
//
// Composes the instance-lock classifier (`classifyInstance`) with a single
// /ping probe into one coarse `status` token an operator or agent can branch
// recovery on. The mapper is a pure function over its inputs — no I/O, no
// network — so the full status matrix is unit-testable without a bridge.
//
// Status vocabulary (copied from Unity Open MCP's bridge-status):
//   - running       — bridge connected, idle.
//   - compiling     — bridge connected, editor compiling.
//   - stopped       — editor not running OR bridge toggle off (no live listener).
//   - unreachable   — editor process alive but the listener did not respond
//                      (usually a transient Live Coding / domain-reload window;
//                      retry shortly).
//   - dead_bridge   — editor process alive but the bridge module failed to
//                      recompile, so /ping will never recover.
//
// Intentional deltas vs Unity (see the tool file + completion notes):
//   - No cold-Safe-Mode process scan. Unity detects a Unity editor running for
//     this project even without a lock (Unity Safe Mode before the bridge's
//     [InitializeOnLoad] wrote a lock) and reuses the dead_bridge token. Unreal
//     has no MCP-side editor-process scanner yet, so a missing lock + a ping
//     failure classifies as `stopped` rather than dead_bridge. When a Phase 8
//     process scan lands, this branch can narrow.
//   - `recoveryHint` points at `unreal_open_mcp_console_get_logs` (P5.3) as the
//     interim offline-recovery surface; Unity points at read_compile_errors.
//     The Unreal offline log/compile-error reader is deferred — the hint is
//     upgraded in place when that tool ships.

import type {
  InstanceClassification,
  InstanceLock,
} from "../instance-discovery.js";

/** Coarse status token an operator/agent branches recovery on. */
export type BridgeStatus =
  | "running"
  | "compiling"
  | "stopped"
  | "unreachable"
  | "dead_bridge";

/**
 * Structured recovery hint — non-null only when a specific next tool exists for
 * the status. `dead_bridge` is the only status that carries one today.
 */
export interface BridgeRecoveryHint {
  /** The tool an agent/operator should call next to diagnose/recover. */
  tool: string;
  /** Why that tool — one short sentence. */
  reason: string;
}

/**
 * The parsed /ping probe outcome passed into the mapper. The caller turns a raw
 * bridge ping `CallToolResult` (or the absence of a probe) into this sum type so
 * the mapper stays pure and network-free.
 *
 *   - { kind: "ok", connected, compiling, isPlaying, ... } — a reachable ping
 *     that returned a health body.
 *   - { kind: "fail" } — the probe failed for any reason (offline / timeout /
 *     HTTP error). The mapper does not need to split these: a failed probe is
 *     only disambiguated by the lock classification.
 *   - null — no probe was run (e.g. a caller that resolves status from the lock
 *     alone). Treated like a failure for routing decisions.
 */
export type PingProbe =
  | {
      kind: "ok";
      connected: boolean;
      compiling?: boolean;
      isPlaying?: boolean;
      unrealVersion?: string | null;
      bridgeVersion?: string;
      mode?: string;
    }
  | { kind: "fail" }
  | null;

/** Inputs to {@link deriveBridgeStatus}. */
export interface DeriveBridgeStatusInput {
  /** Lock classification (`classifyInstance(lock)`). Required. */
  classification: InstanceClassification;
  /** The parsed /ping probe outcome, or null when no probe was run. */
  ping: PingProbe;
}

/** A redacted lock summary surfaced in the result `instance` block. */
export interface InstanceSummary {
  pid: number;
  port: number;
  state: string;
  isCompiling: boolean;
  isPlaying: boolean;
  heartbeatAt: string;
  bridgeVersion: string;
  unrealVersion: string;
}

/**
 * Summarize a lock into the fields the operator/agent cares about. Returns null
 * when there is no lock (the `instance.lock` field is omitted in the result).
 */
export function summarizeInstanceLock(
  lock: InstanceLock | null,
): InstanceSummary | null {
  if (!lock) return null;
  return {
    pid: lock.pid,
    port: lock.port,
    state: lock.state,
    isCompiling: lock.isCompiling,
    isPlaying: lock.isPlaying,
    heartbeatAt: lock.heartbeatAt,
    bridgeVersion: lock.bridgeVersion,
    unrealVersion: lock.unrealVersion,
  };
}

/**
 * The recovery hint for a status, or null when the status has no specific next
 * tool (running / compiling / stopped / unreachable). `dead_bridge` carries a
 * hint pointing at the interim offline-recovery surface (`console_get_logs`);
 * upgraded to a dedicated read_compile_errors tool when that ships.
 */
export function bridgeStatusRecoveryHint(
  status: BridgeStatus,
): BridgeRecoveryHint | null {
  if (status === "dead_bridge") {
    return {
      // Interim: the offline compile-error/log reader is deferred (planned
      // later phase). Until it lands, the Output Log is the only channel that
      // works with the bridge assembly dead — console_get_logs reads the
      // session's GLog ring buffer so an operator can see the compile/Live
      // Coding failure that killed the bridge. Swapped for the dedicated
      // reader in place when it ships.
      tool: "unreal_open_mcp_console_get_logs",
      reason:
        "The editor process is alive but the bridge module is unreachable " +
        "(likely a Live Coding / compile failure). Read the Output Log via " +
        "console_get_logs for the compile errors that killed the bridge — " +
        "the dedicated offline compile-error reader is deferred and will " +
        "replace this hint when it ships.",
    };
  }
  return null;
}

/**
 * A short, human-readable next-step string for each status. Mirrors Unity's
 * bridgeStatusNextStep wording, adapted to Unreal (Live Coding / bridge window)
 * and the interim console recovery hint. Surfaced in the result as `nextStep`.
 */
export function bridgeStatusNextStep(status: BridgeStatus): string {
  switch (status) {
    case "running":
      return "Bridge is ready. Proceed with live-only MCP tools.";
    case "compiling":
      return (
        "The editor is compiling (Live Coding / module reload). Wait for the " +
        "bridge to return to idle, or poll unreal_open_mcp_bridge_status again."
      );
    case "stopped":
      return (
        "The bridge listener is not reachable. Open the Unreal Editor with " +
        "the project loaded and ensure the Unreal Open MCP bridge is running " +
        "(the bridge binds a per-project loopback port; check the instance " +
        "lock at ~/.unreal-open-mcp/instances/<sha256(projectPath)>.json for " +
        "the live port/pid, or set UNREAL_OPEN_MCP_BRIDGE_PORT)."
      );
    case "unreachable":
      return (
        "The bridge listener is not responding but the editor process is " +
        "running — likely a transient Live Coding / domain-reload window. " +
        "Retry shortly; if it persists, read the Output Log via " +
        "unreal_open_mcp_console_get_logs to check for compile errors."
      );
    case "dead_bridge":
      return (
        "The editor process is alive but the bridge module is unreachable " +
        "(likely a Live Coding / compile failure). Call " +
        "unreal_open_mcp_console_get_logs to read the Output Log for the " +
        "compile errors that killed the bridge. Fix the error and let the " +
        "editor reload; the bridge reconnects on the next heartbeat. " +
        "If the editor is NOT reloading, the bridge toggle may be off."
      );
  }
}

/**
 * Derive the coarse bridge `status` from a lock classification + a parsed /ping
 * probe. This is the authoritative status-mapping table for
 * `unreal_open_mcp_bridge_status` (pure, network-free, unit-tested in full).
 *
 * Precedence (first match wins):
 *   1. classification `dead_bridge`           → dead_bridge (PID alive, stale
 *      heartbeat — the bridge assembly is dead regardless of ping).
 *   2. ping ok + compiling                    → compiling.
 *   3. ping ok + connected                    → running.
 *   4. classification `reloading` + ping fail → unreachable (transient reload
 *      window; the listener did not respond).
 *   5. ping fail                              → stopped (no live listener;
 *      editor down OR bridge toggle off OR no matching process scan).
 *
 * Note: Unity adds a cold-Safe-Mode branch (gone + a live editor process for
 * this project via a process scan → dead_bridge). Unreal has no MCP-side editor
 * process scanner yet, so `gone` + ping fail stays `stopped`. That narrows when
 * a process scan lands.
 */
export function deriveBridgeStatus(
  input: DeriveBridgeStatusInput,
): BridgeStatus {
  const { classification, ping } = input;
  const pingReachable = ping !== null && ping.kind === "ok";

  if (classification === "dead_bridge") {
    return "dead_bridge";
  }
  if (pingReachable && ping.kind === "ok") {
    if (ping.compiling === true) return "compiling";
    if (ping.connected) return "running";
  }
  if (!pingReachable && classification === "reloading") {
    return "unreachable";
  }
  return "stopped";
}
