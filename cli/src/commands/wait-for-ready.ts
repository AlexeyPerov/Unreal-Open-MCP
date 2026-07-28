// `unreal-open-mcp-cli wait-for-ready` — poll the bridge until it answers
// /ping (connected, idle) or the overall timeout elapses.
//
// Thin command layer over `lib/port-discovery.ts` (port + token + lock read)
// and `lib/ping-poller.ts` (the poll loop). It:
//   1. resolves the project dir (positional > --project > UNREAL_PROJECT_PATH >
//      cwd), then ABSOLUTIZES it — the port formula hashes the normalized path
//      and a relative path would hash differently than the absolute path the
//      bridge wrote its lock under;
//   2. resolves the bridge port + auth token via the same precedence the
//      mcp-server uses (env override > live instance lock > deterministic
//      hash), so the CLI polls the same port the bridge binds;
//   3. runs the poller;
//   4. prints human or `--json` output and returns the exit code (0 ready, 3
//      timeout/dead-bridge).
//
// The poller does the HTTP + classification; this module is the only place
// that touches stdout/stderr / process.env for the command.

import * as path from "node:path";

import {
  resolvePort,
  resolveAuthToken,
  readInstanceLock,
  type InstanceLock,
} from "../lib/port-discovery.js";
import {
  pollUntilReady,
  DEFAULT_WAIT_TIMEOUT_MS,
  DEFAULT_POLL_INTERVAL_MS,
  PING_FETCH_TIMEOUT_MS,
  type PollOutcome,
} from "../lib/ping-poller.js";
import { PROJECT_PATH_ENV_VAR } from "../constants.js";

/** Exit codes the command can return (kept in sync with README). */
export const EXIT_OK = 0;
export const EXIT_ERROR = 2;
export const EXIT_TIMEOUT = 3;

export interface WaitForReadyCommandOptions {
  /** Parsed CLI flags — the subset `wait-for-ready` consumes. */
  projectPath?: string;
  /** `--port <n>` bridge port override (wins over the lock + hash). */
  port?: number;
  /** `--timeout <ms>` overall wait budget (default 120000). */
  timeout?: number;
  /** `--interval <ms>` sleep between polls (default 2000). */
  interval?: number;
  /** `--json` — emit JSON instead of human-readable output. */
  json?: boolean;
  /** Positional `[projectDir]` arg, if any (wins over --project / env / cwd). */
  positionalProjectDir?: string;
  /** Injectable cwd (default: process.cwd()). */
  cwd?: string;
  /** Injectable env (default: process.env). */
  env?: NodeJS.ProcessEnv;
}

export interface CommandRunOutcome {
  exitCode: number;
}

/**
 * Resolve the project dir, applying the documented precedence (explicit
 * positional > `--project` > `$UNREAL_PROJECT_PATH` > cwd), then ABSOLUTIZE it
 * relative to the injected cwd. The port formula hashes the normalized path,
 * so a relative path must be resolved to absolute first or it would hash
 * differently than the absolute path the bridge wrote its lock under. Pure.
 */
export function resolveProjectDir(opts: WaitForReadyCommandOptions): string {
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
 * Format the poll outcome as a human-readable multi-line block.
 */
export function formatHuman(outcome: PollOutcome, binName: string): string {
  if (outcome.ready) {
    const lines: string[] = [];
    lines.push(
      `Ready after ${outcome.elapsedMs}ms (${outcome.attempts} attempt(s)) — ${outcome.url}`,
    );
    return lines.join("\n") + "\n";
  }
  return `${binName}: not ready: ${outcome.reason} (port ${outcome.port}, ${outcome.attempts} attempt(s))\n`;
}

/**
 * Format the poll outcome as JSON (one line, stable key order).
 */
export function formatJson(outcome: PollOutcome): string {
  return JSON.stringify(outcome) + "\n";
}

/**
 * Run the `wait-for-ready` command. Resolves the bridge port + token via the
 * same precedence the mcp-server uses, then polls until ready or timeout.
 * Returns exit 0 on ready, 3 on timeout / dead-bridge, 2 on a resolve error.
 * Does NOT call process.exit — the dispatcher does, so tests can drive it.
 */
export async function runWaitForReadyCommand(
  opts: WaitForReadyCommandOptions,
  out: (s: string) => Promise<void>,
  err: (s: string) => Promise<void>,
  binName: string,
): Promise<CommandRunOutcome> {
  const projectDir = resolveProjectDir(opts);

  // Read the lock ONCE so the port resolution + the poller's dead-bridge
  // classification share the same snapshot (the bridge rewrites the lock on
  // every start, so reading twice can pair an old port with a new heartbeat).
  const lock: InstanceLock | null = readInstanceLock(projectDir);
  const port = resolvePort(projectDir, opts.port);
  // The bridge defaults to authMode "none" in the MVP, so the poll sends no
  // Authorization header. resolveAuthToken is kept in the resolution path so a
  // future auth-required default lands in one place; it is a no-op until then.
  // (When opts.port is set it short-circuits to undefined; otherwise it would
  // re-read the lock — pass the already-resolved envPort so it does not.)
  void resolveAuthToken(projectDir, opts.port);

  const url = `http://127.0.0.1:${port}/ping`;
  const timeoutMs = positiveInt(opts.timeout, DEFAULT_WAIT_TIMEOUT_MS);
  const intervalMs = positiveInt(opts.interval, DEFAULT_POLL_INTERVAL_MS);

  const outcome = await pollUntilReady(url, port, projectDir, lock, {
    timeoutMs,
    intervalMs,
    fetchTimeoutMs: PING_FETCH_TIMEOUT_MS,
  });

  if (opts.json) {
    // On non-ready, write the JSON envelope to stderr so a `--json` consumer
    // can still parse it while keeping stdout clean for the success stream.
    if (!outcome.ready) {
      await err(formatJson(outcome));
    } else {
      await out(formatJson(outcome));
    }
  } else {
    if (!outcome.ready) {
      await err(formatHuman(outcome, binName));
    } else {
      await out(formatHuman(outcome, binName));
    }
  }

  if (outcome.ready) return { exitCode: EXIT_OK };
  // dead_bridge is a definitive "will not recover" — same exit as timeout.
  return { exitCode: EXIT_TIMEOUT };
}

/** Positive integer guard for `--timeout` / `--interval`. Falls back to `def`. */
function positiveInt(v: number | undefined, def: number): number {
  return typeof v === "number" && Number.isInteger(v) && v > 0 ? v : def;
}
