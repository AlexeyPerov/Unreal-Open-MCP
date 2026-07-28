// AI-agent MCP-client configurator registry for `unreal-open-mcp-cli setup-mcp`.
//
// Adapted from Unity Open MCP's `docs/setup/client-configuration.md` snippet
// table and the agent-id registry pattern used by the sibling CLIs (Unreal-MCP,
// Unity-MCP, Godot-MCP). Each agent knows where its MCP-client config file
// lives and which top-level body key its servers nest under, so the
// stdio-snippet writer (lib/setup-mcp.ts) can merge our `unreal-open-mcp`
// server entry without clobbering sibling servers.
//
// P8.3 scope (MVP, per porting-plan):
//   - stdio transport only — no http, no cloud URL, no OAuth.
//   - agent roster covers the two acceptance-criteria agents (`cursor`,
//     `claude`) plus the JSON-`mcpServers` agents that share Cursor's exact
//     snippet shape (one line of config each). TOML/Codex and the special-case
//     snippet shapes are deferred.
//
// Unreal-specific deltas vs Unity:
//   - Server entry key is `unreal-open-mcp` (Unity uses `unity-open-mcp`).
//   - Project env var is `UNREAL_PROJECT_PATH` (Unity: `UNITY_PROJECT_PATH`).
//   - The published server package is `unreal-open-mcp` (the stdio MCP server
//     bin), NOT this CLI package (`unreal-open-mcp-cli`).

import * as os from "node:os";
import * as path from "node:path";

/**
 * The server-entry key written into every agent's MCP config. Stable across
 * re-runs so an idempotent merge replaces (not duplicates) the prior entry.
 */
export const MCP_SERVER_NAME = "unreal-open-mcp";

/** Serialization format of an agent's config file. */
export type AgentConfigFormat = "json";

/**
 * Per-agent knowledge needed to write a stdio MCP server entry.
 *
 * `getConfigPath` resolves the absolute config-file path for a given project
 * root (or `null` for OS-global config files like Claude Desktop's).
 */
export interface AgentDefinition {
  /** Stable agent id used on the CLI (`setup-mcp <id>`). */
  id: string;
  /** Human-readable name for `--list` and success messages. */
  name: string;
  /** Serialization format of the config file. */
  configFormat: AgentConfigFormat;
  /**
   * Top-level JSON key under which MCP server entries nest
   * (e.g. `mcpServers`, `servers`).
   */
  bodyPath: string;
  /**
   * Short human-readable description of where the config file lives, shown in
   * `--list` (e.g. `<project>/.cursor/mcp.json`, `~/Library/...`).
   */
  configPathDisplay: string;
  /**
   * Resolve the absolute config-file path for a given project root. Returns
   * `null` for OS-global config files that ignore the project root.
   */
  getConfigPath(projectPath: string): string | null;
}

// ---------------------------------------------------------------------------
// Platform helpers — centralised so the registry entries stay declarative.
// ---------------------------------------------------------------------------

function home(): string {
  return os.homedir();
}

function appData(): string {
  return process.env.APPDATA ?? path.join(home(), "AppData", "Roaming");
}

function isWindows(): boolean {
  return process.platform === "win32";
}

// ---------------------------------------------------------------------------
// Registry
//
// MVP roster: the two acceptance-criteria agents plus the JSON-`mcpServers`
// agents that share Cursor's exact stdio snippet shape. Adding an agent is one
// entry — the snippet writer derives the server entry from `bodyPath` +
// `MCP_SERVER_NAME` + the shared stdio props (lib/setup-mcp.ts).
// ---------------------------------------------------------------------------

export const agentRegistry: readonly AgentDefinition[] = [
  {
    id: "cursor",
    name: "Cursor",
    configFormat: "json",
    bodyPath: "mcpServers",
    configPathDisplay: "<project>/.cursor/mcp.json",
    getConfigPath: (p) => path.join(p, ".cursor", "mcp.json"),
  },
  {
    id: "claude",
    name: "Claude Desktop",
    configFormat: "json",
    bodyPath: "mcpServers",
    configPathDisplay: "OS global (~/Library/Application Support/Claude/...)",
    getConfigPath: () => {
      if (isWindows()) {
        return path.join(appData(), "Claude", "claude_desktop_config.json");
      }
      if (process.platform === "darwin") {
        return path.join(
          home(),
          "Library",
          "Application Support",
          "Claude",
          "claude_desktop_config.json",
        );
      }
      return path.join(home(), ".config", "Claude", "claude_desktop_config.json");
    },
  },
  {
    id: "claude-code",
    name: "Claude Code",
    configFormat: "json",
    bodyPath: "mcpServers",
    configPathDisplay: "<project>/.mcp.json",
    getConfigPath: (p) => path.join(p, ".mcp.json"),
  },
  {
    id: "vscode-copilot",
    name: "Visual Studio Code (Copilot)",
    configFormat: "json",
    bodyPath: "servers",
    configPathDisplay: "<project>/.vscode/mcp.json",
    getConfigPath: (p) => path.join(p, ".vscode", "mcp.json"),
  },
  {
    id: "gemini",
    name: "Gemini CLI",
    configFormat: "json",
    bodyPath: "mcpServers",
    configPathDisplay: "<project>/.gemini/settings.json",
    getConfigPath: (p) => path.join(p, ".gemini", "settings.json"),
  },
  {
    id: "cline",
    name: "Cline",
    configFormat: "json",
    bodyPath: "mcpServers",
    configPathDisplay: "OS global (Code/User/globalStorage/saoudrizwan.claude-dev/...)",
    getConfigPath: () => {
      if (isWindows()) {
        return path.join(
          appData(),
          "Code",
          "User",
          "globalStorage",
          "saoudrizwan.claude-dev",
          "settings",
          "cline_mcp_settings.json",
        );
      }
      const base = process.platform === "darwin"
        ? path.join(home(), "Library", "Application Support", "Code", "User", "globalStorage")
        : path.join(home(), ".config", "Code", "User", "globalStorage");
      return path.join(base, "saoudrizwan.claude-dev", "settings", "cline_mcp_settings.json");
    },
  },
] as const;

// ---------------------------------------------------------------------------
// Lookup helpers
// ---------------------------------------------------------------------------

/** Find an agent by id. Returns `undefined` when unknown. Pure. */
export function getAgentById(id: string): AgentDefinition | undefined {
  return agentRegistry.find((a) => a.id === id);
}

/** Every registered agent id, in registry order. Pure. */
export function getAgentIds(): string[] {
  return agentRegistry.map((a) => a.id);
}
