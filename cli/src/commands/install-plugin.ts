// `unreal-open-mcp-cli install-plugin` — copy (+ enable) the bridge and verify
// plugins into an Unreal project.
//
// Thin command layer over `lib/install-plugin.ts`. It:
//   1. resolves the project dir (positional > --project > UNREAL_PROJECT_PATH >
//      cwd), mirroring the global-option precedence the arg parser advertises;
//   2. maps parsed CLI flags to the library's `InstallPluginOptions`;
//   3. runs the installer;
//   4. prints human or `--json` output and returns the exit code.
//
// The library does no I/O of its own beyond the install; this module is the
// only place that touches stdout/stderr / process.env for the command, so it
// stays unit-testable by injecting both.

import { installPlugin, type InstallPluginResult } from "../lib/install-plugin.js";
import { PROJECT_PATH_ENV_VAR } from "../constants.js";

/** Exit codes the command can return (kept in sync with README). */
export const EXIT_OK = 0;
export const EXIT_WARNING = 1;
export const EXIT_ERROR = 2;

export interface InstallPluginCommandOptions {
  /** Parsed CLI flags — the subset `install-plugin` consumes. */
  projectPath?: string;
  pluginSource?: string;
  symlink?: boolean;
  withVerify?: boolean;
  dryRun?: boolean;
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
 * Resolve the project dir the install targets, applying the documented
 * precedence: explicit positional > `--project` > `$UNREAL_PROJECT_PATH` > cwd.
 * Pure (no I/O).
 */
export function resolveProjectDir(opts: InstallPluginCommandOptions): string {
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
 * Format the install result as a human-readable multi-line block.
 */
export function formatHuman(result: InstallPluginResult, binName: string): string {
  if (!result.ok) {
    return `${binName}: install-plugin failed: ${result.message}\n`;
  }
  const lines: string[] = [];
  const tag = result.dryRun ? "[dry-run] " : "";
  lines.push(`${tag}Installed into ${result.projectDir} (source: ${result.sourceRoot}):`);
  for (const p of result.installed) {
    lines.push(`  - ${p.name}  [${p.mode}]  -> ${p.installedPath}`);
  }
  lines.push(
    `  .uproject ${result.uprojectMutated ? "updated" : "already enabled (not rewritten)"}.`,
  );
  if (result.warnings.length > 0) {
    lines.push("Warnings:");
    for (const w of result.warnings) lines.push(`  - ${w}`);
  }
  if (!result.dryRun) {
    lines.push("");
    lines.push("Next: rebuild / open the project in the Unreal Editor, then run:");
    lines.push(`  ${binName} setup-mcp`);
  }
  return lines.join("\n") + "\n";
}

/**
 * Format the install result as JSON (one line, stable key order).
 */
export function formatJson(result: InstallPluginResult): string {
  return JSON.stringify(result) + "\n";
}

/**
 * Run the `install-plugin` command. Writes human or JSON output to stdout (and
 * the error message to stderr on failure) and returns the exit code. Does NOT
 * call process.exit — the dispatcher does, so tests can drive it.
 */
export async function runInstallPluginCommand(
  opts: InstallPluginCommandOptions,
  out: (s: string) => Promise<void>,
  err: (s: string) => Promise<void>,
  binName: string,
): Promise<CommandRunOutcome> {
  const projectDir = resolveProjectDir(opts);
  const result = await installPlugin({
    projectDir,
    pluginSource: opts.pluginSource,
    symlink: opts.symlink,
    withVerify: opts.withVerify,
    dryRun: opts.dryRun,
  });

  if (opts.json) {
    // On failure, write the JSON envelope to stderr so a `--json` consumer can
    // still parse it while keeping stdout clean for the success stream.
    if (!result.ok) {
      await err(formatJson(result));
      return { exitCode: EXIT_ERROR };
    }
    await out(formatJson(result));
  } else {
    if (!result.ok) {
      await err(formatHuman(result, binName));
      return { exitCode: EXIT_ERROR };
    }
    await out(formatHuman(result, binName));
  }

  if (!result.ok) return { exitCode: EXIT_ERROR };
  // Success path: warnings (e.g. verify missing, symlink fallback) are non-fatal
  // and ride in the result envelope; the CLI exits 0 so a script can chain.
  return { exitCode: EXIT_OK };
}
