// `unreal-open-mcp-cli status` — report project / plugin / port / lock / bridge
// readiness in one shot.
//
// Thin command layer over `lib/port-discovery.ts` (port + lock + classify),
// `lib/ping-poller.ts` (a single /ping probe — NOT the poll loop), and
// `lib/status.ts` (the pure status-mapping + report composition that mirrors
// the MCP `unreal_open_mcp_bridge_status` tool). It:
//   1. resolves the project dir (positional > --project > UNREAL_PROJECT_PATH >
//      cwd), then ABSOLUTIZES it — the port formula hashes the normalized path;
//   2. reads the instance lock ONCE + resolves the bridge port via the same
//      precedence the mcp-server uses;
//   3. optionally fires one /ping probe (--no-probe skips it);
//   4. composes the status report (project / plugin / port / lock / bridge) and
//      prints human or --json output.
//
// Status NEVER fails solely because the editor is down — `unreachable` /
// `stopped` are successful reports (exit 0), mirroring the MCP bridge_status
// philosophy. Only a project-dir resolution error exits non-zero.

import * as path from "node:path";

import {
  resolvePort,
  readInstanceLock,
  classifyInstance,
  lockPath,
  type InstanceLock,
  type InstanceClassification,
} from "../lib/port-discovery.js";
import { singlePing, PING_FETCH_TIMEOUT_MS, type PingBody } from "../lib/ping-poller.js";
import {
  composeStatusReport,
  detectPlugins,
  type PingProbe,
  type StatusReport,
} from "../lib/status.js";
import { PROJECT_PATH_ENV_VAR } from "../constants.js";

/** Exit codes the command can return (kept in sync with README). */
export const EXIT_OK = 0;
export const EXIT_ERROR = 2;

export interface StatusCommandOptions {
  /** Parsed CLI flags — the subset `status` consumes. */
  projectPath?: string;
  /** `--port <n>` bridge port override (wins over the lock + hash). */
  port?: number;
  /** `--no-probe` — skip the live /ping probe. */
  noProbe?: boolean;
  /** `--json` — emit JSON instead of human-readable output. */
  json?: boolean;
  /** Positional `[projectDir]` arg, if any (wins over --project / env / cwd). */
  positionalProjectDir?: string;
  /** Injectable cwd (default: process.cwd()). */
  cwd?: string;
  /** Injectable env (default: process.env). */
  env?: NodeJS.ProcessEnv;
  /**
   * Fetch implementation for the /ping probe; injectable for tests. Defaults to
   * the global fetch (Node 18+).
   */
  fetchImpl?: typeof fetch;
}

export interface CommandRunOutcome {
  exitCode: number;
}

/**
 * Resolve the project dir, applying the documented precedence (explicit
 * positional > `--project` > `$UNREAL_PROJECT_PATH` > cwd), then ABSOLUTIZE it
 * relative to the injected cwd (the port formula hashes the normalized path).
 * Pure. Mirrors `wait-for-ready`'s resolver so the two commands agree.
 */
export function resolveProjectDir(opts: StatusCommandOptions): string {
  const cwd = opts.cwd ?? process.cwd();
  const env = opts.env ?? process.env;
  const dir =
    opts.positionalProjectDir ??
    opts.projectPath ??
    env[PROJECT_PATH_ENV_VAR] ??
    cwd;
  return path.isAbsolute(dir) ? dir : path.resolve(cwd, dir);
}

/**
 * Translate a {@link singlePing} result into the pure mapper's {@link PingProbe}.
 * `ready` / `compiling` carry a parsed body → ok; `offline` / `error` → fail.
 */
export function pingResultToProbe(
  status: "ready" | "compiling" | "offline" | "error",
  body: PingBody | null,
): { probe: PingProbe; body: PingBody | null } {
  if (status === "ready" || status === "compiling") {
    if (body) {
      return {
        probe: {
          kind: "ok",
          connected: body.connected === true,
          compiling: typeof body.compiling === "boolean" ? body.compiling : undefined,
          isPlaying: typeof body.isPlaying === "boolean" ? body.isPlaying : undefined,
          unrealVersion: typeof body.unrealVersion === "string" ? body.unrealVersion : null,
          bridgeVersion: typeof body.bridgeVersion === "string" ? body.bridgeVersion : undefined,
          mode: typeof body.mode === "string" ? body.mode : undefined,
        },
        body,
      };
    }
    // A ready/compiling status with no body (e.g. a 503) is reachable but the
    // mapper only needs reachable + compiling; synthesize a minimal ok probe.
    return {
      probe: { kind: "ok", connected: status === "ready", compiling: status === "compiling" },
      body: null,
    };
  }
  return { probe: { kind: "fail" }, body: null };
}

/**
 * Format the status report as a human-readable multi-line block.
 */
export function formatHuman(report: StatusReport): string {
  const lines: string[] = [];
  lines.push(`Project:   ${report.projectPath}`);
  lines.push(`Port:      ${report.port}  (${report.baseUrl})`);
  const bridge = report.plugin.bridgeInstalled ? "installed" : "not installed";
  const verify = report.plugin.verifyInstalled ? "installed" : "not installed";
  lines.push(`Plugin:    bridge ${bridge}, verify ${verify}`);
  lines.push(`Instance:  ${report.instance.classification}${report.instance.lock ? ` (pid ${report.instance.lock.pid}, ${report.instance.lock.state})` : " (no lock)"}`);
  const reachable = report.bridge.reachable ? "reachable" : "not reachable";
  lines.push(`Bridge:    ${report.bridge.status} (${reachable})`);
  return lines.join("\n") + "\n";
}

/** Format the status report as JSON (one line, stable key order). */
export function formatJson(report: StatusReport): string {
  return JSON.stringify(report) + "\n";
}

/**
 * Run the `status` command. Composes the report from a single lock snapshot +
 * (optionally) one /ping probe, then prints it. Returns exit 0 on any
 * successful report (offline is success); exit 2 only on a resolution error.
 * Does NOT call process.exit — the dispatcher does, so tests can drive it.
 */
export async function runStatusCommand(
  opts: StatusCommandOptions,
  out: (s: string) => Promise<void>,
  _err: (s: string) => Promise<void>,
  _binName: string,
): Promise<CommandRunOutcome> {
  const projectDir = resolveProjectDir(opts);

  // Read the lock ONCE so port resolution + classification share one snapshot.
  const lock: InstanceLock | null = readInstanceLock(projectDir);
  const port = resolvePort(projectDir, opts.port);
  const classification: InstanceClassification = classifyInstance(lock);
  const onDiskLockPath = lockPath(projectDir);
  const plugins = detectPlugins(projectDir);

  let probe: PingProbe = null;
  let pingBody: PingBody | null = null;
  let probed = false;

  if (!opts.noProbe) {
    probed = true;
    const url = `http://127.0.0.1:${port}/ping`;
    const fetchImpl = opts.fetchImpl ?? fetch;
    const result = await singlePing(url, fetchImpl, PING_FETCH_TIMEOUT_MS);
    const mapped = pingResultToProbe(result.status, result.body);
    probe = mapped.probe;
    pingBody = mapped.body;
  }

  const report = composeStatusReport({
    projectPath: projectDir,
    port,
    lock,
    classification,
    lockPath: onDiskLockPath,
    plugins,
    ping: probe,
    pingBody,
    probed,
  });

  await out(opts.json ? formatJson(report) : formatHuman(report));
  return { exitCode: EXIT_OK };
}
