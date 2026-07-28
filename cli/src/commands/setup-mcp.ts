// `unreal-open-mcp-cli setup-mcp` — write a stdio MCP client config snippet
// for a supported AI agent (Cursor, Claude Desktop, ...).
//
// Thin command layer over `lib/setup-mcp.ts`. It:
//   1. handles `--list` (print supported agent ids + config paths, then exit 0);
//   2. resolves the project dir (positional > --project > UNREAL_PROJECT_PATH >
//      cwd) AND absolutizes it — the snippet's UNREAL_PROJECT_PATH must be
//      absolute or the agent's spawn cwd breaks the path;
//   3. maps parsed CLI flags to the library's `SetupMcpOptions`;
//   4. runs the snippet writer (write or --dry-run);
//   5. prints human or `--json` output and returns the exit code.
//
// The library does the JSON merge + disk write; this module is the only place
// that touches stdout/stderr / process.env for the command, so it stays
// unit-testable by injecting both.

import * as path from "node:path";

import { setupMcp, type SetupMcpResult } from "../lib/setup-mcp.js";
import { agentRegistry } from "../lib/agents.js";
import { PROJECT_PATH_ENV_VAR } from "../constants.js";
import { readPackageVersion } from "../package-version.js";

/** Exit codes the command can return (kept in sync with README). */
export const EXIT_OK = 0;
export const EXIT_WARNING = 1;
export const EXIT_ERROR = 2;

export interface SetupMcpCommandOptions {
  /** Parsed CLI flags — the subset `setup-mcp` consumes. */
  projectPath?: string;
  /** Bridge port override (`--port`). Optional; only written when set. */
  port?: number;
  /** `--server-command <cmd>` override. */
  serverCommand?: string;
  /** `--dry-run` — print snippet, write nothing. */
  dryRun?: boolean;
  /** `--list` — print agent table and exit 0 (ignores other flags). */
  list?: boolean;
  /** `--json` — emit JSON instead of human-readable output. */
  json?: boolean;
  /** Positional `[agent]` arg (wins over the implicit single-agent case). */
  positionalAgent?: string;
  /** Injectable cwd (default: process.cwd()). */
  cwd?: string;
  /** Injectable env (default: process.env). */
  env?: NodeJS.ProcessEnv;
}

export interface CommandRunOutcome {
  exitCode: number;
}

/**
 * Resolve the project dir the snippet targets, applying the documented
 * precedence: explicit `--project` > `$UNREAL_PROJECT_PATH` > cwd, then
 * absolutize it (the snippet's env block requires an absolute path or the
 * agent's spawn cwd breaks resolution). Pure (no I/O).
 */
export function resolveProjectDir(opts: SetupMcpCommandOptions): string {
  const cwd = opts.cwd ?? process.cwd();
  const env = opts.env ?? process.env;
  const dir = opts.projectPath ?? env[PROJECT_PATH_ENV_VAR] ?? cwd;
  // Resolve relative to the injected cwd (NOT process.cwd()) so the absolute
  // path is deterministic in tests and the agent spawn finds the project.
  return path.isAbsolute(dir) ? dir : path.resolve(cwd, dir);
}

/**
 * Format the agent table for `--list` (human). Sorted by id so the output is
 * stable across registry edits. Column widths are computed from the registry
 * so longer ids (e.g. `vscode-copilot`) never overflow the layout.
 */
export function formatAgentList(): string {
  const sorted = [...agentRegistry].sort((a, b) => a.id.localeCompare(b.id));
  const idHeader = "ID";
  const nameHeader = "NAME";
  const pathHeader = "CONFIG PATH";
  const wId = Math.max(idHeader.length, ...sorted.map((a) => a.id.length));
  const wName = Math.max(nameHeader.length, ...sorted.map((a) => a.name.length));
  const header = `  ${idHeader.padEnd(wId)}  ${nameHeader.padEnd(wName)}  ${pathHeader}`;
  const sep = "  " + "-".repeat(header.length - 2);
  const rows = sorted.map(
    (a) => `  ${a.id.padEnd(wId)}  ${a.name.padEnd(wName)}  ${a.configPathDisplay}`,
  );
  return [
    "Supported MCP agents:",
    "",
    header,
    sep,
    ...rows,
    "",
    "Run: unreal-open-mcp-cli setup-mcp <id> --project <projectDir>",
  ].join("\n") + "\n";
}

/**
 * Format the result as a human-readable multi-line block.
 */
export function formatHuman(result: SetupMcpResult, binName: string): string {
  if (!result.ok) {
    return `${binName}: setup-mcp failed: ${result.message}\n`;
  }
  const lines: string[] = [];
  const tag = !result.written ? "[dry-run] " : "";
  lines.push(
    `${tag}${result.agentName} MCP config ${result.written ? "written to" : "for"} ${result.configPath}`,
  );
  lines.push("");
  lines.push("Snippet:");
  // Indent every snippet line by two spaces so the block reads as a quoted
  // config body in the terminal.
  for (const line of result.snippet.split("\n")) {
    lines.push(`  ${line}`);
  }
  if (result.warnings.length > 0) {
    lines.push("");
    lines.push("Warnings:");
    for (const w of result.warnings) lines.push(`  - ${w}`);
  }
  lines.push("");
  lines.push("Next:");
  for (const s of result.nextSteps) lines.push(`  - ${s}`);
  return lines.join("\n") + "\n";
}

/**
 * Format the result as JSON (one line, stable key order).
 */
export function formatJson(result: SetupMcpResult): string {
  return JSON.stringify(result) + "\n";
}

/**
 * Run the `setup-mcp` command. Handles `--list` itself; otherwise delegates to
 * the library. Writes human or JSON output to stdout (and the error message to
 * stderr on failure) and returns the exit code. Does NOT call process.exit.
 */
export async function runSetupMcpCommand(
  opts: SetupMcpCommandOptions,
  out: (s: string) => Promise<void>,
  err: (s: string) => Promise<void>,
  binName: string,
): Promise<CommandRunOutcome> {
  // --list short-circuits before any project-path / agent requirement.
  if (opts.list) {
    if (opts.json) {
      // Minimal JSON envelope so a script can enumerate the registry.
      const body = JSON.stringify({
        agents: [...agentRegistry]
          .sort((a, b) => a.id.localeCompare(b.id))
          .map((a) => ({
            id: a.id,
            name: a.name,
            configFormat: a.configFormat,
            bodyPath: a.bodyPath,
            configPathDisplay: a.configPathDisplay,
          })),
      }) + "\n";
      await out(body);
    } else {
      await out(formatAgentList());
    }
    return { exitCode: EXIT_OK };
  }

  const agentId = opts.positionalAgent;
  if (!agentId) {
    const msg =
      `setup-mcp requires an agent id. Run '${binName} setup-mcp --list' to see the supported agents.`;
    await err(`${binName}: ${msg}\n`);
    return { exitCode: EXIT_ERROR };
  }

  const projectDir = resolveProjectDir(opts);
  const result = await setupMcp(
    {
      agentId,
      projectDir,
      serverCommand: opts.serverCommand,
      bridgePort: opts.port,
      dryRun: opts.dryRun,
    },
    readPackageVersion(),
  );

  if (opts.json) {
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
  return { exitCode: EXIT_OK };
}
