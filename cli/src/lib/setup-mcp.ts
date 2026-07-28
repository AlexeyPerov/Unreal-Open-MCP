// `setup-mcp` — write a stdio MCP client config snippet for a supported AI
// agent (Cursor, Claude Desktop, ...) so it can spawn `unreal-open-mcp` against
// a project without hand-editing JSON.
//
// Adapted from Unity Open MCP's `docs/setup/client-configuration.md` snippet
// table (the per-agent body key + config path) and the JSON-merge contract used
// by the sibling CLIs. Deltas (per P8.3 plan):
//   - stdio transport only — no http, no cloud URL, no OAuth, no token fields.
//   - Server package is `unreal-open-mcp` (the stdio MCP server bin); the
//     project env var is `UNREAL_PROJECT_PATH`.
//   - The server command defaults to `npx -y unreal-open-mcp@<cli-version>` so
//     the CLI and server stay version-locked; `--server-command` overrides it
//     for the monorepo-dev case (`node <repo>/mcp-server/dist/index.js`).
//
// Merge contract: a re-run deep-merges by server key. Sibling MCP servers in
// the same config file are preserved byte-for-byte; only the
// `unreal-open-mcp` entry is replaced. A malformed existing file is treated as
// empty (a warning is surfaced) so the command never clobbers a user's
// hand-edited config with bad data — it starts fresh under `mcpServers`/the
// agent's body key.

import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

import { PORT_ENV_VAR, PROJECT_PATH_ENV_VAR } from "../constants.js";
import {
  MCP_SERVER_NAME,
  getAgentById,
  type AgentDefinition,
} from "./agents.js";
import { readPackageVersion } from "../package-version.js";

/** Fallback server version when the CLI's own version cannot be read. */
const FALLBACK_SERVER_VERSION = "latest";

/**
 * The published MCP server package (the stdio bin). Pinned to the CLI's own
 * version on release so the two ship together; the bin name matches
 * `mcp-server/package.json#bin`.
 */
export const MCP_SERVER_PACKAGE = "unreal-open-mcp";

export interface SetupMcpOptions {
  /** Agent id (cursor, claude, ...). Must be a registered id. */
  agentId: string;
  /**
   * Absolute project root (the `.uproject` parent). Required — the snippet
   * writes `UNREAL_PROJECT_PATH` and a relative path would break agent spawn.
   * The command layer resolves + absolutizes this so the library stays pure-ish.
   */
  projectDir: string;
  /**
   * `--server-command <cmd>` override. Wins over the default `npx -y
   * unreal-open-mcp@<version>` vector. Pass the bare command name only
   * (`node`, `unreal-open-mcp`) — the args vector is derived.
   */
  serverCommand?: string;
  /**
   * Optional bridge port. When set, added to the snippet's `env` block as
   * `UNREAL_OPEN_MCP_BRIDGE_PORT`. When omitted, the env var is NOT written
   * (per the porting-plan: do not invent ports).
   */
  bridgePort?: number;
  /** `--dry-run` — compute + return the snippet, write nothing. */
  dryRun?: boolean;
}

/** A resolved server-entry `command` + `args` vector. */
export interface ServerCommandVector {
  command: string;
  args: string[];
}

/**
 * Resolve the stdio server-entry command + args.
 *
 * Precedence:
 *   1. `--server-command <cmd>` — the caller's bare command (`node`,
 *      `unreal-open-mcp`). When the command is `node`, we assume the
 *      monorepo-dev case and point at `mcp-server/dist/index.js` (resolved
 *      against the CLI's own location, since the CLI ships inside the monorepo
 *      in dev and as a sibling package on publish). For any other command we
 *      emit the bare command with NO args (a global install, e.g.
 *      `unreal-open-mcp`).
 *   2. default — `npx -y unreal-open-mcp@<cli-version>`.
 *
 * Pure. The `cliVersion` is read once by the caller (or defaulted) so this
 * function is deterministic in tests.
 */
export function resolveServerCommand(
  opts: Pick<SetupMcpOptions, "serverCommand">,
  cliVersion: string,
  moduleUrl: string = import.meta.url,
): ServerCommandVector {
  const override = opts.serverCommand?.trim();
  if (override) {
    if (override === "node") {
      // Monorepo-dev case: launch the workspace-built server. `dist/index.js`
      // is resolved relative to this CLI module so it works from a local
      // checkout (`cli/dist/lib/setup-mcp.js` → `../../mcp-server/dist/index.js`).
      const here = path.dirname(fileURLToPath(moduleUrl));
      const serverEntry = path.resolve(
        here,
        "..",
        "..",
        "mcp-server",
        "dist",
        "index.js",
      );
      return { command: "node", args: [serverEntry] };
    }
    // A bare global command (e.g. a globally-installed `unreal-open-mcp`).
    return { command: override, args: [] };
  }
  const ver = cliVersion || FALLBACK_SERVER_VERSION;
  return { command: "npx", args: ["-y", `${MCP_SERVER_PACKAGE}@${ver}`] };
}

/**
 * Build the per-agent server-entry object that gets merged into the config's
 * `<bodyPath>.<MCP_SERVER_NAME>` slot. Always stdio for the MVP — the env
 * block carries `UNREAL_PROJECT_PATH` (required) and optionally the bridge port.
 *
 * Pure. Same shape for every JSON-`mcpServers` agent (Cursor, Claude, ...).
 */
export function buildServerEntry(
  projectDir: string,
  vector: ServerCommandVector,
  bridgePort?: number,
): Record<string, unknown> {
  const env: Record<string, string> = {
    [PROJECT_PATH_ENV_VAR]: projectDir,
  };
  if (bridgePort !== undefined) {
    env[PORT_ENV_VAR] = String(bridgePort);
  }
  return {
    command: vector.command,
    args: vector.args,
    env,
  };
}

export type SetupMcpErrorKind =
  | "unknown_agent" // agentId not in the registry
  | "project_dir_required"; // projectDir empty / not absolute

export interface SetupMcpError {
  kind: SetupMcpErrorKind;
  message: string;
}

export interface SetupMcpResultSuccess {
  ok: true;
  /** Agent id the snippet was written for. */
  agentId: string;
  /** Human-readable agent name. */
  agentName: string;
  /** Absolute config-file path the snippet was written to (or would be). */
  configPath: string;
  /** The serialized snippet (merged config body, 2-space indent + newline). */
  snippet: string;
  /** Whether the snippet was written to disk (`false` under --dry-run). */
  written: boolean;
  /** Non-fatal warnings (malformed existing config, etc.). */
  warnings: string[];
  /** Next-step hints appended to the human output. */
  nextSteps: string[];
}

export type SetupMcpResult =
  | SetupMcpResultSuccess
  | ({ ok: false } & SetupMcpError);

/**
 * Merge `entry` into the `<bodyPath>.<serverName>` slot of the JSON config at
 * `configPath`, preserving every other server entry, and return the serialized
 * content. When `dryRun` is true the merged content is computed and returned
 * but NOTHING is written (side-effect-free — no dir creation, no file write).
 *
 * A malformed existing file is treated as empty (the caller surfaces a warning)
 * so the command never silently wipes a user's hand-edited config.
 */
export function mergeJsonAgentConfig(
  configPath: string,
  bodyPath: string,
  serverName: string,
  entry: Record<string, unknown>,
  dryRun: boolean,
): { content: string; warning?: string } {
  let root: Record<string, unknown> = {};
  let warning: string | undefined;

  if (fs.existsSync(configPath)) {
    try {
      const parsed = JSON.parse(fs.readFileSync(configPath, "utf8"));
      if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
        root = parsed as Record<string, unknown>;
      } else {
        warning = `${configPath}: existing config is not a JSON object; starting fresh under '${bodyPath}'.`;
        root = {};
      }
    } catch {
      warning = `${configPath}: existing config is malformed JSON; starting fresh under '${bodyPath}'.`;
      root = {};
    }
  }

  // Navigate/create the body key (e.g. mcpServers).
  let body = root[bodyPath] as Record<string, unknown> | undefined;
  if (!body || typeof body !== "object" || Array.isArray(body)) {
    body = {};
    root[bodyPath] = body;
  }

  // Replace the named server entry wholesale (idempotent re-run; drops stale
  // transport keys the prior snippet may have carried). Sibling servers under
  // the same body key are preserved, as are unrelated top-level keys.
  body[serverName] = entry;
  root[bodyPath] = body;

  const content = JSON.stringify(root, null, 2) + "\n";
  if (!dryRun) {
    const dir = path.dirname(configPath);
    if (!fs.existsSync(dir)) {
      fs.mkdirSync(dir, { recursive: true });
    }
    fs.writeFileSync(configPath, content);
  }
  return { content, warning };
}

/**
 * Resolve + write (or print, under `--dry-run`) the agent's stdio MCP snippet.
 * Never throws — failures are structured `ok:false` results so the CLI surfaces
 * a clean exit code + message.
 */
export async function setupMcp(
  opts: SetupMcpOptions,
  cliVersion: string = readPackageVersion(),
): Promise<SetupMcpResult> {
  const agent = getAgentById(opts.agentId);
  if (!agent) {
    return {
      ok: false,
      kind: "unknown_agent",
      message: `Unknown agent "${opts.agentId}".`,
    };
  }

  if (!opts.projectDir || !path.isAbsolute(opts.projectDir)) {
    return {
      ok: false,
      kind: "project_dir_required",
      message: `projectDir must be an absolute path (got: "${opts.projectDir ?? ""}").`,
    };
  }

  const warnings: string[] = [];
  const nextSteps: string[] = [];

  const configPath = resolveConfigPath(agent, opts.projectDir);
  if (!configPath) {
    // The MVP registry has no OS-global-only agents that lack a path, but keep
    // the guard so a future entry can opt out cleanly.
    return {
      ok: false,
      kind: "unknown_agent",
      message: `Agent "${agent.id}" does not resolve a config path.`,
    };
  }

  const vector = resolveServerCommand(opts, cliVersion);
  const entry = buildServerEntry(opts.projectDir, vector, opts.bridgePort);
  const { content, warning } = mergeJsonAgentConfig(
    configPath,
    agent.bodyPath,
    MCP_SERVER_NAME,
    entry,
    !!opts.dryRun,
  );
  if (warning) warnings.push(warning);

  nextSteps.push(`Restart ${agent.name} so it reloads the MCP config.`);
  nextSteps.push(
    `If the bridge plugin is not installed yet, run: unreal-open-mcp-cli install-plugin --project ${opts.projectDir}`,
  );
  nextSteps.push(
    "Then open the project in the Unreal Editor and run: unreal-open-mcp-cli wait-for-ready",
  );

  return {
    ok: true,
    agentId: agent.id,
    agentName: agent.name,
    configPath,
    snippet: content,
    written: !opts.dryRun,
    warnings,
    nextSteps,
  };
}

/**
 * Resolve the agent's config-file path for the given project root. Centralised
 * so OS-global agents (Claude Desktop) can ignore the project root uniformly.
 */
function resolveConfigPath(
  agent: AgentDefinition,
  projectPath: string,
): string | null {
  return agent.getConfigPath(projectPath);
}
