// Bridge status composition for the `status` CLI command.
//
// Copy fidelity from
// mcp-server/src/tools/bridge-status-helpers.ts (deriveBridgeStatus,
// summarizeInstanceLock, BridgeStatus, PingProbe) +
// mcp-server/src/index.ts#resolveBridgeStatus (the result body shape the MCP
// `unreal_open_mcp_bridge_status` tool returns). The CLI is a standalone
// publishable package with no runtime deps (ADR-007), so it cannot import the
// mcp-server package; this module duplicates the THIN status-mapping surface
// the `status` command needs, keeping the cross-side contract pinned by the
// same status-matrix tests (see status.test.ts).
//
// Keeping the mapper pure + network-free (no I/O) lets the full status matrix
// be unit-tested without a bridge: the command layer turns the on-disk lock +
// a single /ping probe into the inputs, then this module composes the report.
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
// Intentional delta vs the mcp-server: no cold-Safe-Mode process scan. The
// mcp-server can detect a Unity/Unreal editor running for this project even
// without a lock and reuse the dead_bridge token; the CLI has no MCP-side
// editor-process scanner, so a missing lock + a ping failure classifies as
// `stopped` rather than dead_bridge (mirrors the mcp-server helper's own note).

import { existsSync } from "node:fs";
import { basename, join } from "node:path";

import type {
  InstanceClassification,
  InstanceLock,
} from "./port-discovery.js";
import { BRIDGE_PLUGIN_NAME, VERIFY_PLUGIN_NAME } from "./plugin-source.js";
import type { PingBody } from "./ping-poller.js";

/** Coarse status token an operator/agent branches recovery on. */
export type BridgeStatus =
  | "running"
  | "compiling"
  | "stopped"
  | "unreachable"
  | "dead_bridge";

/**
 * The parsed /ping probe outcome passed into the mapper. The command layer
 * turns a single {@link singlePing} result (or the absence of a probe) into
 * this sum type so the mapper stays pure and network-free.
 *
 *   - { kind: "ok", connected, compiling, ... } — a reachable ping that
 *     returned a health body.
 *   - { kind: "fail" } — the probe failed for any reason (offline / timeout /
 *     HTTP error). The mapper does not split these: a failed probe is only
 *     disambiguated by the lock classification.
 *   - null — no probe was run (--no-probe). Treated like a failure for routing
 *     decisions (status is derived from the lock alone).
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

/** A redacted lock summary surfaced in the report `instance` block. */
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
 * Summarize a lock into the fields an operator/agent cares about. Returns null
 * when there is no lock (the `instance.lock` field is omitted in the report).
 * Copy of mcp-server's summarizeInstanceLock — kept in lockstep.
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
 * Derive the coarse bridge `status` from a lock classification + a parsed /ping
 * probe. This is the authoritative status-mapping table — a copy of the
 * mcp-server's deriveBridgeStatus (pure, network-free, unit-tested in full).
 *
 * Precedence (first match wins):
 *   1. classification `dead_bridge`           → dead_bridge (PID alive, stale
 *      heartbeat — the bridge assembly is dead regardless of ping).
 *   2. ping ok + compiling                    → compiling.
 *   3. ping ok + connected                    → running.
 *   4. classification `reloading` + ping fail → unreachable (transient reload
 *      window; the listener did not respond).
 *   5. ping fail                              → stopped (no live listener;
 *      editor down OR bridge toggle off).
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

// ---------------------------------------------------------------------------
// Plugin presence
//
// `status` reports whether the bridge (+ verify) plugin is installed into the
// project's Plugins/ folder. This mirrors what install-plugin writes: a
// <project>/Plugins/<Name>/<Name>.uplugin descriptor per plugin.

/** Descriptor filenames each plugin ships (mirror plugin-source.ts). */
const BRIDGE_DESCRIPTOR = "UnrealOpenMCP.uplugin";
const VERIFY_DESCRIPTOR = "UnrealOpenMCPVerify.uplugin";

/**
 * Absolute path to a plugin's descriptor under `<project>/Plugins/<Name>/`, if
 * it exists. Returns null otherwise. Pure over the filesystem snapshot.
 */
function pluginDescriptorPath(
  projectDir: string,
  name: string,
  descriptor: string,
): string | null {
  const p = join(projectDir, "Plugins", name, descriptor);
  return existsSync(p) ? p : null;
}

/** Result of a plugin-presence probe. */
export interface PluginPresence {
  /** The bridge plugin descriptor exists under Plugins/. */
  bridgeInstalled: boolean;
  /** The verify plugin descriptor exists under Plugins/. */
  verifyInstalled: boolean;
  /** Resolved bridge descriptor path (when installed), else null. */
  bridgeDescriptorPath: string | null;
  /** Resolved verify descriptor path (when installed), else null. */
  verifyDescriptorPath: string | null;
}

/**
 * Probe whether the bridge (+ verify) plugin is installed into the project.
 * Pure over the filesystem snapshot; never throws.
 */
export function detectPlugins(projectDir: string): PluginPresence {
  const bridgeDescriptorPath = pluginDescriptorPath(
    projectDir,
    BRIDGE_PLUGIN_NAME,
    BRIDGE_DESCRIPTOR,
  );
  const verifyDescriptorPath = pluginDescriptorPath(
    projectDir,
    VERIFY_PLUGIN_NAME,
    VERIFY_DESCRIPTOR,
  );
  return {
    bridgeInstalled: bridgeDescriptorPath !== null,
    verifyInstalled: verifyDescriptorPath !== null,
    bridgeDescriptorPath,
    verifyDescriptorPath,
  };
}

// ---------------------------------------------------------------------------
// Report composition

/** Inputs to {@link composeStatusReport}. */
export interface ComposeStatusInput {
  /** Absolute project root. */
  projectPath: string;
  /** Resolved bridge port (env > live lock > hash — resolved by the caller). */
  port: number;
  /** The already-read instance lock snapshot, or null when none. */
  lock: InstanceLock | null;
  /** Lock classification (`classifyInstance(lock)`), supplied by the caller. */
  classification: InstanceClassification;
  /** The path to the instance lock file on disk (or null). */
  lockPath: string | null;
  /** Plugin-presence probe result. */
  plugins: PluginPresence;
  /** The parsed /ping probe outcome, or null when --no-probe skipped it. */
  ping: PingProbe;
  /** Raw /ping body captured on a reachable probe (for the `body` field), or null. */
  pingBody: PingBody | null;
  /** Whether a live probe was attempted (false under --no-probe). */
  probed: boolean;
}

/** The `instance` block of the status report. */
export interface StatusInstanceBlock {
  lockPath: string | null;
  classification: InstanceClassification;
  lock: InstanceSummary | null;
}

/** The `bridge` block of the status report. */
export interface StatusBridgeBlock {
  status: BridgeStatus;
  /** True when the probe reached a health body. False on fail or --no-probe. */
  reachable: boolean;
  /** Whether a live probe was attempted. */
  probed: boolean;
  /** The /ping body when reachable, else null. */
  body: PingBody | null;
}

/** The full `status` report object (JSON envelope). */
export interface StatusReport {
  command: "status";
  projectPath: string;
  projectName: string;
  port: number;
  baseUrl: string;
  plugin: PluginPresence;
  instance: StatusInstanceBlock;
  bridge: StatusBridgeBlock;
}

/**
 * Compose the full status report from a lock snapshot + a parsed /ping probe.
 * Pure over its inputs — no I/O. The command layer supplies every signal.
 */
export function composeStatusReport(input: ComposeStatusInput): StatusReport {
  const status = deriveBridgeStatus({
    classification: input.classification,
    ping: input.ping,
  });
  const reachable = input.ping !== null && input.ping.kind === "ok";
  return {
    command: "status",
    projectPath: input.projectPath,
    projectName: basename(input.projectPath) || input.projectPath,
    port: input.port,
    baseUrl: `http://127.0.0.1:${input.port}`,
    plugin: input.plugins,
    instance: {
      lockPath: input.lockPath,
      classification: input.classification,
      lock: summarizeInstanceLock(input.lock),
    },
    bridge: {
      status,
      reachable,
      probed: input.probed,
      body: reachable ? input.pingBody : null,
    },
  };
}
