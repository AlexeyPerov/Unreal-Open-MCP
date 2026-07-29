// `unreal-open-mcp-cli configure` — read/write the project-local Open MCP
// settings file (`<project>/.unreal-open-mcp/settings.json`).
//
// Thin command layer over `lib/configure.ts` (read/write + deep-merge). It:
//   1. resolves the project dir (positional > --project > UNREAL_PROJECT_PATH >
//      cwd), then ABSOLUTIZES it;
//   2. applies the requested patch (--bridge-port <n> sets, --clear-bridge-port
//      clears) via the library's idempotent atomic write;
//   3. prints human or --json output.
//
// Local-only. No cloud host / cloud URL / connection-mode / token fields
// (ADR-001). The MVP key is `bridgePort`.

import * as path from "node:path";

import {
  readSettings,
  writeSettings,
  settingsPath,
  type ProjectSettings,
} from "../lib/configure.js";
import { PROJECT_PATH_ENV_VAR } from "../constants.js";

/** Exit codes the command can return (kept in sync with README). */
export const EXIT_OK = 0;
export const EXIT_ERROR = 2;

export interface ConfigureCommandOptions {
  /** Parsed CLI flags — the subset `configure` consumes. */
  projectPath?: string;
  /** `--bridge-port <n>` — set the bridge port override. */
  bridgePort?: number;
  /** `--clear-bridge-port` — clear (delete) the bridge port override. */
  clearBridgePort?: boolean;
  /** `--dry-run` — resolve + report, write nothing. */
  dryRun?: boolean;
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
 * positional > `--project` > `$UNREAL_PROJECT_PATH` > cwd), then ABSOLUTIZE it.
 * Pure. Mirrors the other commands' resolver.
 */
export function resolveProjectDir(opts: ConfigureCommandOptions): string {
  const cwd = opts.cwd ?? process.cwd();
  const env = opts.env ?? process.env;
  const dir =
    opts.positionalProjectDir ??
    opts.projectPath ??
    env[PROJECT_PATH_ENV_VAR] ??
    cwd;
  return path.isAbsolute(dir) ? dir : path.resolve(cwd, dir);
}

/** The result envelope a successful configure emits. */
export interface ConfigureResult {
  command: "configure";
  projectPath: string;
  settingsPath: string;
  /** The settings now persisted (post-patch); under --dry-run, the pre-patch read. */
  settings: ProjectSettings;
  /** Whether the settings directory was created by this run. */
  createdDir: boolean;
  /** True when the run was a --dry-run (nothing written). */
  dryRun: boolean;
}

/**
 * Format the configure result as a human-readable multi-line block.
 */
export function formatHuman(result: ConfigureResult): string {
  const lines: string[] = [];
  lines.push(`Settings:  ${result.settingsPath}`);
  const action = result.dryRun ? "would write" : "wrote";
  const port = result.settings.bridgePort;
  lines.push(`Bridge port: ${typeof port === "number" ? String(port) : "(unset)"} (${action})`);
  if (result.createdDir) lines.push("(created settings directory)");
  return lines.join("\n") + "\n";
}

/** Format the configure result as JSON (one line, stable key order). */
export function formatJson(result: ConfigureResult): string {
  return JSON.stringify(result) + "\n";
}

/**
 * Run the `configure` command. Validates the flag combination, applies the
 * patch via the library, and prints the result. Returns exit 0 on success,
 * exit 2 on a flag conflict or a settings I/O error. Does NOT call
 * process.exit — the dispatcher does, so tests can drive it.
 */
export async function runConfigureCommand(
  opts: ConfigureCommandOptions,
  out: (s: string) => Promise<void>,
  err: (s: string) => Promise<void>,
  binName: string,
): Promise<CommandRunOutcome> {
  // Flag conflict: setting + clearing at once is ambiguous.
  if (opts.bridgePort !== undefined && opts.clearBridgePort) {
    await err(`${binName}: --bridge-port and --clear-bridge-port are mutually exclusive.\n`);
    return { exitCode: EXIT_ERROR };
  }
  // No-op when neither flag is passed is allowed: it prints the current settings
  // (a read), which is useful for inspection. The result carries the read state.

  const projectDir = resolveProjectDir(opts);

  // Dry-run: read only, report the would-be state.
  if (opts.dryRun) {
    const read = readSettings(projectDir);
    if (!read.ok) {
      await err(`${binName}: ${read.message}\n`);
      return { exitCode: EXIT_ERROR };
    }
    const projected = applyPatch(read.settings, opts);
    const result: ConfigureResult = {
      command: "configure",
      projectPath: projectDir,
      settingsPath: settingsPath(projectDir),
      settings: projected,
      createdDir: false,
      dryRun: true,
    };
    await out(opts.json ? formatJson(result) : formatHuman(result));
    return { exitCode: EXIT_OK };
  }

  // No mutation requested: report the current settings as a read.
  if (opts.bridgePort === undefined && !opts.clearBridgePort) {
    const read = readSettings(projectDir);
    if (!read.ok) {
      await err(`${binName}: ${read.message}\n`);
      return { exitCode: EXIT_ERROR };
    }
    const result: ConfigureResult = {
      command: "configure",
      projectPath: projectDir,
      settingsPath: settingsPath(projectDir),
      settings: read.settings,
      createdDir: false,
      dryRun: false,
    };
    await out(opts.json ? formatJson(result) : formatHuman(result));
    return { exitCode: EXIT_OK };
  }

  const patch = buildPatch(opts);
  const written = writeSettings(projectDir, patch);
  if (!written.ok) {
    await err(`${binName}: ${written.message}\n`);
    return { exitCode: EXIT_ERROR };
  }

  const result: ConfigureResult = {
    command: "configure",
    projectPath: projectDir,
    settingsPath: written.path,
    settings: written.settings,
    createdDir: written.createdDir,
    dryRun: false,
  };
  await out(opts.json ? formatJson(result) : formatHuman(result));
  return { exitCode: EXIT_OK };
}

/** Build the settings patch from the parsed flags. */
function buildPatch(opts: ConfigureCommandOptions): { bridgePort?: number | null } {
  if (opts.clearBridgePort) return { bridgePort: null };
  if (opts.bridgePort !== undefined) return { bridgePort: opts.bridgePort };
  return {};
}

/** Apply a flag patch onto existing settings (pure; used for --dry-run projection). */
function applyPatch(
  settings: ProjectSettings,
  opts: ConfigureCommandOptions,
): ProjectSettings {
  if (opts.clearBridgePort) {
    const next = { ...settings };
    delete next.bridgePort;
    return next;
  }
  if (opts.bridgePort !== undefined) {
    return { ...settings, bridgePort: opts.bridgePort };
  }
  return settings;
}
