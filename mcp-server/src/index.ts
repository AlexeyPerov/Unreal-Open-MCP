#!/usr/bin/env node

// stdio MCP server for Unreal Open MCP.
//
// P1.7 scope: boot a Model Context Protocol server over stdio, expose the tool
// registry (now containing `unreal_open_mcp_ping`), and dispatch tool calls
// through the LiveClient into the live bridge's `GET /ping`. Instance discovery
// (P1.6) resolves the bridge port + auth token at startup; the LiveClient (new
// in P1.7) is the single HTTP hop for live-routed tools.
//
// There is no tool-group filtering, offline/local routing, or CLI dispatch in
// this task — those land in later phases. The server is the stdio hop an AI
// client connects to; `unreal_open_mcp_ping` is the first end-to-end probe
// (stdio → instance discovery → HTTP → bridge).
//
// Adapted from Unity Open MCP's mcp-server/src/index.ts (copy fidelity), with
// intentional deltas documented at the bottom of this file.

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  ListToolsRequestSchema,
  CallToolRequestSchema,
  type CallToolResult,
  type ListToolsRequest,
  type CallToolRequest,
} from "@modelcontextprotocol/sdk/types.js";
import { pathToFileURL } from "node:url";
import { ALL_TOOLS } from "./tools/index.js";
import { readPackageVersion } from "./package-version.js";
import {
  resolvePort,
  resolveAuthToken,
  PORT_OVERRIDE_ENV_VAR,
  readInstanceLock,
  isPidAlive,
} from "./instance-discovery.js";
import { LiveClient, type Router } from "./live-client.js";

/** Name advertised in the MCP `initialize` response. */
export const SERVER_NAME = "unreal-open-mcp";

/** Mandatory env var: the Unreal project path the server is bound to. */
export const PROJECT_PATH_ENV_VAR = "UNREAL_PROJECT_PATH";

// Read the version from package.json at runtime so `npm version` and the
// version-sync flow keep the reported server version in sync without editing
// this source file.
const PACKAGE_VERSION = readPackageVersion();

/** Name → tool lookup, built once for call dispatch + unknown-tool diagnostics. */
const TOOL_BY_NAME = new Map(ALL_TOOLS.map((t) => [t.name, t]));

/**
 * Module-level live router. Installed once by `main()` after env/port/token
 * resolution so the `tools/call` handler can dispatch live-routed tools without
 * threading the client through every handler closure. When unset (no client
 * installed — e.g. some unit tests exercise handlers directly without booting
 * the full wiring), known tools fall back to a "not wired" error rather than
 * crashing; unknown-tool handling is unaffected.
 *
 * Reset via {@link resetLiveRouterForTest} in tests so cases that assert the
 * fallback path aren't poisoned by a previous test's install.
 */
let liveRouter: Router | null = null;

/**
 * Install the live router. Called once from `main()`. Exported so tests that
 * want to drive `handleCallTool` against a stub bridge can install their own.
 */
export function setLiveRouter(router: Router | null): void {
  liveRouter = router;
}

/**
 * Test helper to clear the live router between cases. Not part of the runtime
 * contract — exported only because the fallback-path test needs a clean slate.
 */
export function resetLiveRouterForTest(): void {
  liveRouter = null;
}

/**
 * tools/list handler. Returns the visible tool set. The registry is empty in
 * P1.5; per-session group filtering lands later. Exported so unit tests can
 * call it directly without booting a stdio transport.
 */
export async function handleListTools(_request?: ListToolsRequest) {
  return { tools: ALL_TOOLS };
}

/**
 * tools/call handler. Dispatches by name; an unknown name returns a structured
 * MCP error result (`isError: true`) listing the registered tool names so the
 * agent can self-correct. A known name is routed through the installed
 * {@link liveRouter} (LiveClient); when no router is installed (not yet wired
 * by `main()`, or cleared in tests), known tools fall back to a "not wired"
 * error instead of crashing. Exported for direct unit tests.
 */
export async function handleCallTool(
  request: CallToolRequest,
): Promise<CallToolResult> {
  const { name, arguments: args } = request.params;
  const tool = TOOL_BY_NAME.get(name);
  if (!tool) {
    const known = ALL_TOOLS.map((t) => t.name);
    const suffix =
      known.length > 0
        ? ` Registered tools: ${known.join(", ")}.`
        : " No tools are registered yet.";
    return {
      isError: true,
      content: [{ type: "text", text: `Unknown tool: ${name}.${suffix}` }],
    };
  }
  if (liveRouter) {
    return liveRouter.route(name, args ?? {});
  }
  // No live router installed — e.g. a unit test exercising handlers directly.
  // Keeps the call honest rather than silently succeeding.
  return {
    isError: true,
    content: [
      {
        type: "text",
        text: `Tool ${name} has no handler wired yet (scaffold).`,
      },
    ],
  };
}

/**
 * Build the MCP server with the list/call handlers wired. Exported so tests
 * can construct a server against an in-memory transport if needed. The server
 * name and version come from the package; capabilities advertise
 * `tools.listChanged` so later-phase group activation can signal clients.
 */
export function createServer(): Server {
  const server = new Server(
    { name: SERVER_NAME, version: PACKAGE_VERSION },
    { capabilities: { tools: { listChanged: true } } },
  );

  server.setRequestHandler(ListToolsRequestSchema, handleListTools);
  server.setRequestHandler(CallToolRequestSchema, handleCallTool);

  return server;
}

/**
 * Resolve env for the MCP server. `UNREAL_PROJECT_PATH` is mandatory — without
 * a project there is nothing to route to. Exit 1 with a clear message if it is
 * missing.
 *
 * Bridge port resolution uses P1.6 instance discovery:
 *   1. UNREAL_OPEN_MCP_BRIDGE_PORT env var (override wins; users who pin a
 *      port keep working as before)
 *   2. ~/.unreal-open-mcp/instances/<hash>.json lock file (when its pid is alive)
 *   3. deterministic hash of the project path (20000 + sha256 % 10000)
 *
 * The resolved port is logged with its source (override / lock / hash) so
 * users can see which bridge was picked. The auth token is discovered from
 * the same lock when present; until P5.6 the bridge omits it and the token
 * resolves to undefined (the LiveClient wiring lands in P1.7).
 */
function getEnv(): {
  projectPath: string;
  port: number;
  authToken?: string;
  envPort?: number;
} {
  const projectPath = process.env[PROJECT_PATH_ENV_VAR];
  if (!projectPath) {
    console.error(
      `${SERVER_NAME}: ${PROJECT_PATH_ENV_VAR} environment variable is required.`,
    );
    process.exit(1);
  }

  const rawEnvPort = process.env[PORT_OVERRIDE_ENV_VAR];
  const parsedEnvPort = rawEnvPort ? parseInt(rawEnvPort, 10) : undefined;
  const envPort =
    rawEnvPort && Number.isInteger(parsedEnvPort) ? parsedEnvPort : undefined;

  const port = resolvePort(projectPath, envPort);

  // Surface which precedence branch supplied the port. The lock branch is only
  // credited when the lock is actually live (pid alive) — resolvePort already
  // validated that internally; we re-check here purely for the log label so
  // the two code paths don't diverge on what "lock" means.
  let source: string;
  if (typeof envPort === "number") {
    source = "env override";
  } else {
    const lock = readInstanceLock(projectPath);
    if (lock && typeof lock.port === "number" && isPidAlive(lock.pid)) {
      source = "instance lock";
    } else {
      source = "hash fallback";
    }
  }

  console.error(`[${SERVER_NAME}] Bound to project: ${projectPath}`);
  console.error(
    `[${SERVER_NAME}] Bridge port resolved to ${port} (${source})`,
  );

  const authToken = resolveAuthToken(
    projectPath,
    Number.isInteger(parsedEnvPort) ? parsedEnvPort : undefined,
  );
  if (authToken) {
    console.error(`[${SERVER_NAME}] Bridge auth token discovered from instance lock.`);
  } else {
    console.error(
      `[${SERVER_NAME}] No bridge auth token discovered (bridge authMode must be "none").`,
    );
  }

  return { projectPath, port, authToken, envPort };
}

async function main(): Promise<void> {
  // Resolve project + bridge port + auth token at startup, then install the
  // LiveClient so the tools/call handler can dispatch live-routed tools
  // (`unreal_open_mcp_ping` is the first; other tools land in later phases).
  const env = getEnv();
  setLiveRouter(
    new LiveClient(env.port, env.authToken, env.projectPath),
  );
  const server = createServer();
  const transport = new StdioServerTransport();
  // StdioServerTransport closes when stdin EOFs (client disconnect). Once the
  // transport closes we tear the server down; with no remaining event-loop
  // handles the Node process exits 0 — the "clean exit on disconnect"
  // contract. Logging goes to stderr so it never corrupts the stdio JSON-RPC
  // stream on stdout.
  transport.onclose = async () => {
    try {
      await server.close();
    } catch (err) {
      console.error(`${SERVER_NAME}: error closing server:`, err);
    }
    process.exit(0);
  };
  await server.connect(transport);
}

// Entrypoint guard: only boot the stdio server when this file is the process
// entrypoint, not when it is imported by a test. Comparing URL forms (not raw
// argv strings) is the cross-platform ESM idiom and survives path/extension
// differences between platforms.
const entrypointUrl = process.argv[1]
  ? pathToFileURL(process.argv[1]).href
  : "";
if (entrypointUrl === import.meta.url) {
  main().catch((err) => {
    console.error(`${SERVER_NAME} fatal:`, err);
    process.exit(1);
  });
}

// Intentional deltas from Unity Open MCP (mcp-server/src/index.ts):
//  - Server name `unreal-open-mcp` (not `unity-open-mcp`).
//  - `UNREAL_PROJECT_PATH` env var (not `UNITY_PROJECT_PATH`).
//  - Port resolution + instance discovery + auth token wired in P1.6; the
//    LiveClient is installed in P1.7 so live-routed tools
//    (`unreal_open_mcp_ping`) dispatch through it. The port source label is
//    more granular than Unity's ("env override" / "instance lock" / "hash
//    fallback" vs Unity's "env override" / "instance discovery") so users can
//    tell whether a live lock supplied the port.
//  - LiveClient is the minimal P1.7 surface (ping only; `tool_not_routed` for
//    other names). No BatchSpawn / ToolRouter / offline / local routing /
//    resources / CLI dispatch yet.
//  - Handlers (`handleListTools`, `handleCallTool`) are exported standalone so
//    tests can exercise them directly. Unity inlines them inside `createServer`
//    and tests other modules; we export them so the dispatch + fallback paths
//    are unit-testable without booting stdio. The live router is a module-level
//    holder with a setter so `main()` installs the real client while tests can
//    install a stub or clear it via `resetLiveRouterForTest`.
//  - Explicit `transport.onclose` → `server.close()` → `process.exit(0)` to
//    make the "clean exit on disconnect" contract observable rather than
//    relying solely on the event loop draining.
