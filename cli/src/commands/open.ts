// `unreal-open-mcp-cli open` — launch the Unreal Editor for a project with the
// bridge loaded.
//
// Thin command layer over `lib/open.ts`. It:
//   1. resolves the project dir (positional > --project > UNREAL_PROJECT_PATH >
//      cwd), mirroring the global-option precedence the arg parser advertises;
//   2. maps parsed CLI flags to the library's `OpenProjectOptions`;
//   3. runs the launcher;
//   4. prints human or `--json` output and returns the exit code.
//
// The library does the engine resolution + spawn; this module is the only place
// that touches stdout/stderr / process.env for the command, so it stays
// unit-testable by injecting both.

import * as path from "node:path";

import { openProject, type OpenProjectResult } from "../lib/open.js";
import { PROJECT_PATH_ENV_VAR, PORT_ENV_VAR } from "../constants.js";

/** Exit codes the command can return (kept in sync with README). */
export const EXIT_OK = 0;
export const EXIT_WARNING = 1;
export const EXIT_ERROR = 2;

export interface OpenCommandOptions {
  /** Parsed CLI flags — the subset `open` consumes. */
  projectPath?: string;
  /** `--engine-root <dir>` override. */
  engineRoot?: string;
  /** `--no-build` (accepted for forward compat; MVP does not invoke UBT). */
  noBuild?: boolean;
  /** `--port <n>` bridge port override, propagated to the editor launch arg. */
  port?: number;
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
 * Resolve the project dir the launch targets, applying the documented
 * precedence: explicit positional > `--project` > `$UNREAL_PROJECT_PATH` > cwd.
 * Pure (no I/O).
 */
export function resolveProjectDir(opts: OpenCommandOptions): string {
  const cwd = opts.cwd ?? process.cwd();
  const env = opts.env ?? process.env;
  return (
    opts.positionalProjectDir ??
    opts.projectPath ??
    env[PROJECT_PATH_ENV_VAR] ??
    cwd
  );
}

/**
 * Format the launch result as a human-readable multi-line block.
 */
export function formatHuman(result: OpenProjectResult, binName: string): string {
  if (!result.success) {
    return `${binName}: open failed: ${result.errorMessage}\n`;
  }
  const lines: string[] = [];
  lines.push(`Launched Unreal Editor (PID: ${result.editorPid ?? "unknown"})`);
  lines.push(`  editor:  ${result.editorPath}`);
  lines.push(`  engine:  ${result.engineRoot}  [via ${result.engineSource}]`);
  lines.push(`  project: ${result.projectDir}`);
  if (result.envVars[PORT_ENV_VAR]) {
    lines.push(`  bridge:  port ${result.envVars[PORT_ENV_VAR]} (override)`);
  }
  if (result.warnings.length > 0) {
    lines.push("Warnings:");
    for (const w of result.warnings) lines.push(`  - ${w}`);
  }
  lines.push("");
  lines.push("Next: wait for the bridge to bind, then run:");
  lines.push(`  ${binName} wait-for-ready`);
  return lines.join("\n") + "\n";
}

/**
 * Format the launch result as JSON (one line, stable key order). The discriminated
 * union serializes with the `kind`/`success` discriminants intact.
 */
export function formatJson(result: OpenProjectResult): string {
  return JSON.stringify(result) + "\n";
}

/**
 * Run the `open` command. Writes human or JSON output to stdout (and the error
 * message to stderr on failure) and returns the exit code. Does NOT call
 * process.exit — the dispatcher does, so tests can drive it.
 */
export async function runOpenCommand(
  opts: OpenCommandOptions,
  out: (s: string) => Promise<void>,
  err: (s: string) => Promise<void>,
  binName: string,
): Promise<CommandRunOutcome> {
  const projectDir = resolveProjectDir(opts);
  const result = await openProject({
    projectDir,
    engineRoot: opts.engineRoot,
    noBuild: opts.noBuild,
    bridgePort: opts.port,
  });

  if (opts.json) {
    if (!result.success) {
      await err(formatJson(result));
      return { exitCode: EXIT_ERROR };
    }
    await out(formatJson(result));
  } else {
    if (!result.success) {
      await err(formatHuman(result, binName));
      return { exitCode: EXIT_ERROR };
    }
    await out(formatHuman(result, binName));
  }

  if (!result.success) return { exitCode: EXIT_ERROR };
  return { exitCode: EXIT_OK };
}
