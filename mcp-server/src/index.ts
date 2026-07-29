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
  computePort,
  isUsablePort,
  authTokenFromLock,
  PORT_OVERRIDE_ENV_VAR,
  readInstanceLock,
  isPidAlive,
} from "./instance-discovery.js";
import { LiveClient, type Router } from "./live-client.js";
import { ToolRouter, routePolicy } from "./tool-router.js";

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
 * Module-level dispatch router (P8.6). Installed once by `main()` after
 * env/port/token resolution so the `tools/call` handler can delegate every
 * call — local, offline, batch, and live alike — through a single policy
 * table. The router owns the local handlers (capabilities, bridge_status) and
 * POSTs live tools through the injected {@link LiveClient}. When unset (no
 * router installed — e.g. a unit test exercising handlers directly), known
 * tools fall back to a "not wired" error rather than crashing; unknown-tool
 * handling is unaffected.
 *
 * Reset via {@link resetLiveRouterForTest} in tests so cases that assert the
 * fallback path aren't poisoned by a previous test's install.
 */
let toolRouter: ToolRouter | null = null;

/**
 * The Unreal project path the server is bound to, set by {@link getEnv} during
 * `main()`. Threaded into the {@link ToolRouter} so the local `bridge_status`
 * handler can read this project's instance lock without re-resolving env at
 * call time. Exported so unit tests can point it at a temp dir to plant/read
 * an instance lock without going through `main()`.
 */
let boundProjectPath: string | null = null;

/**
 * Set the bound project path used by {@link ToolRouter}'s bridge_status
 * handler to read the instance lock. Installed by `main()` after env
 * resolution; exported so tests can drive bridge_status against a temp project
 * without booting stdio.
 */
export function setBoundProjectPath(projectPath: string | null): void {
  boundProjectPath = projectPath;
}

/**
 * Install the live transport. Called once from `main()`; wraps the supplied
 * transport in a {@link ToolRouter} bound to {@link boundProjectPath} so the
 * `tools/call` handler delegates every call through the single dispatch spine.
 * Exported so tests that want to drive `handleCallTool` against a stub bridge
 * can install their own transport.
 */
export function setLiveRouter(router: Router | null): void {
  toolRouter = router ? new ToolRouter(router, boundProjectPath) : null;
}

/**
 * Test helper to clear the dispatch router between cases. Not part of the
 * runtime contract — exported only because the fallback-path test needs a
 * clean slate.
 */
export function resetLiveRouterForTest(): void {
  toolRouter = null;
}

/**
 * Resolve a local-route tool call in-process via a transient {@link ToolRouter}.
 * Returns `null` when the tool is NOT a local-route tool (the caller then
 * routes it through the live transport).
 *
 * `router` is the live transport to probe through (the same one `handleCallTool`
 * dispatches live tools through). It is optional and threaded in only so
 * `bridge_status` can fire its /ping probe through the installed transport;
 * capabilities ignores it. When null, bridge_status treats the ping as failed
 * (it still reports a status from the lock alone). Exported so unit tests can
 * drive the local dispatch directly without booting the live transport — the
 * router is built per-call from the passed transport + the module-level
 * {@link boundProjectPath}, matching the production wiring.
 */
export async function handleLocalTool(
  name: string,
  args: Record<string, unknown>,
  router: Router | null = null,
): Promise<CallToolResult | null> {
  if (routePolicy(name) !== "local") {
    return null;
  }
  // A throwaway router bound to the same project path the production router
  // uses. The local handlers never escape to the live transport except for the
  // bridge_status /ping probe, so a null router degrades to a lock-only status
  // (identical to the pre-P8.6 behavior).
  const transient = new ToolRouter(
    router ?? NOT_WIRED_ROUTER,
    boundProjectPath,
  );
  return transient.route(name, args);
}

/**
 * Stand-in transport for {@link handleLocalTool} when no live router is
 * installed. capabilities never touches it; bridge_status treats any probe as
 * failed (the expected lock-only path). It never throws on construction — the
 * probe's own try/catch turns the call into `{ kind: "fail" }`.
 */
const NOT_WIRED_ROUTER: Router = {
  async route() {
    return {
      isError: true,
      content: [
        { type: "text", text: '{"error":{"code":"not_wired"}}' },
      ],
    };
  },
};

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
  // P8.6 — every call goes through the ToolRouter dispatch spine. The router
  // resolves local-route tools in-process (capabilities works with the editor
  // down; bridge_status composes the lock classifier + one /ping probe) and
  // POSTs live tools through the installed transport, stamping `_source` +
  // `_route` metadata on every JSON result. When no router is installed (a
  // unit test exercising handlers directly), local tools still resolve via the
  // throwaway-router path in {@link handleLocalTool}; live/offline/batch tools
  // fall through to the "not wired" error below.
  if (toolRouter) {
    return toolRouter.route(name, args ?? {});
  }
  // No dispatch router installed (e.g. a unit test exercising handlers
  // directly). Local-route tools still resolve in-process so a test with no
  // router can call capabilities / bridge_status; everything else surfaces a
  // "not wired" error rather than silently succeeding.
  const localResult = await handleLocalTool(name, args ?? {}, null);
  if (localResult !== null) {
    return localResult;
  }
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
 * Parse UNREAL_OPEN_MCP_BRIDGE_PORT into a usable port, or undefined.
 *
 * Applies the FULL validation resolvePort uses — integer, in 1..65535, and no
 * trailing garbage (`parseInt("20000abc")` is 20000, which would silently
 * "accept" a typo'd value).
 */
function parseOverridePort(raw: string | undefined): number | undefined {
  if (!raw) return undefined;
  const trimmed = raw.trim();
  if (!/^\d+$/.test(trimmed)) return undefined;
  const parsed = Number(trimmed);
  return isUsablePort(parsed) ? parsed : undefined;
}

/**
 * Resolve env for the MCP server. `UNREAL_PROJECT_PATH` is mandatory — without
 * a project there is nothing to route to. Exit 1 with a clear message if it is
 * missing.
 *
 * Bridge port resolution uses P1.6 instance discovery:
 *   1. UNREAL_OPEN_MCP_BRIDGE_PORT env var (override wins; users who pin a
 *      port keep working as before)
 *   2. ~/.unreal-open-mcp/instances/<hash>.json lock file (when its pid is alive
 *      and its port is a valid TCP port)
 *   3. deterministic hash of the project path (20000 + sha256 % 10000)
 *
 * The resolved port is logged with its source (override / lock / hash) so
 * users can see which bridge was picked. The auth token is discovered from
 * the same lock when present; the LiveClient attaches it as a Bearer header
 * on every request. When the lock omits the token (older bridge) or an env
 * port override is in use, the token resolves to undefined and no
 * Authorization header is sent (the bridge must then be in authMode "none").
 *
 * The lock is read exactly ONCE here and the port, token, and log label are all
 * derived from that single snapshot. The bridge rewrites the lock and rotates
 * the token on every start, so separate reads could pair an old port with a new
 * token and 401 every request until this process restarts.
 */
function getEnv(): {
  projectPath: string;
  port: number;
  authToken?: string;
} {
  const projectPath = process.env[PROJECT_PATH_ENV_VAR];
  if (!projectPath) {
    console.error(
      `${SERVER_NAME}: ${PROJECT_PATH_ENV_VAR} environment variable is required.`,
    );
    process.exit(1);
  }

  // Parse the env override with the SAME predicate resolvePort applies
  // (integer AND in 1..65535). Checking only integer-ness meant
  // UNREAL_OPEN_MCP_BRIDGE_PORT=0 or =70000 was reported as "(env override)" in
  // the log while resolvePort had correctly fallen back to the lock/hash — the
  // exact diagnostic an operator reads when debugging a port mismatch. The
  // trailing-garbage check also rejects "20000abc", which parseInt accepts.
  const rawEnvPort = process.env[PORT_OVERRIDE_ENV_VAR];
  const envPort = parseOverridePort(rawEnvPort);

  // Read the instance lock ONCE and derive the port, the token, and the log
  // label from that single snapshot. The bridge rewrites the lock and rotates
  // the token on every start, so separate reads could pair an old port with a
  // new token and 401 every request until the MCP server restarts.
  const lock = readInstanceLock(projectPath);

  let port: number;
  let source: string;
  if (envPort !== undefined) {
    port = envPort;
    source = "env override";
  } else if (lock && isUsablePort(lock.port) && isPidAlive(lock.pid)) {
    port = lock.port;
    source = "instance lock";
  } else {
    port = computePort(projectPath);
    source = "hash fallback";
  }

  console.error(`[${SERVER_NAME}] Bound to project: ${projectPath}`);
  console.error(
    `[${SERVER_NAME}] Bridge port resolved to ${port} (${source})`,
  );

  // An explicit env port means there is no lock to trust for the token either
  // (the bridge that wrote the lock may be a different instance).
  const authToken =
    envPort !== undefined ? undefined : authTokenFromLock(lock);
  if (authToken) {
    console.error(`[${SERVER_NAME}] Bridge auth token discovered from instance lock.`);
  } else {
    console.error(
      `[${SERVER_NAME}] No bridge auth token discovered (bridge authMode must be "none").`,
    );
  }

  // Record the bound project path so bridge_status (P5.7) can read this
  // project's instance lock without re-resolving env at call time.
  setBoundProjectPath(projectPath);

  return { projectPath, port, authToken };
}

async function main(): Promise<void> {
  // Resolve project + bridge port + auth token at startup, then install the
  // dispatch router. `getEnv()` first records the bound project path; the
  // ToolRouter (installed next via setLiveRouter) captures it so the local
  // bridge_status handler can read this project's instance lock. The router is
  // the single dispatch spine for every tools/call (local + live; offline/batch
  // stubs refuse until their handlers land).
  const env = getEnv();
  setLiveRouter(
    new LiveClient(env.port, env.authToken, env.projectPath),
  );
  const server = createServer();
  const transport = new StdioServerTransport();
  // Clean exit on client disconnect.
  //
  // `transport.onclose` alone was NOT enough: the SDK's StdioServerTransport
  // only registers "data" and "error" listeners on stdin, and invokes onclose
  // exclusively from its own close() — nothing calls close() on stdin EOF. So
  // this handler never ran on a real disconnect, and the process only exited
  // because the event loop happened to drain. Any lingering handle (e.g. an
  // in-flight tool fetch's abort timer) left the process alive after the client
  // was gone.
  //
  // Hooking stdin "end" gives us the actual EOF signal; onclose is kept for
  // programmatic closes. `shutdown` is idempotent so both paths are safe.
  // Logging goes to stderr so it never corrupts the JSON-RPC stream on stdout.
  let shuttingDown = false;
  const shutdown = async () => {
    if (shuttingDown) return;
    shuttingDown = true;
    try {
      await server.close();
    } catch (err) {
      console.error(`${SERVER_NAME}: error closing server:`, err);
    }
    process.exit(0);
  };

  transport.onclose = shutdown;
  process.stdin.once("end", () => void shutdown());
  process.stdin.once("close", () => void shutdown());

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
//    other names). No BatchSpawn / offline routing / resources / CLI dispatch
//    yet. P3.8 added the first **local-route** tool
//    (`unreal_open_mcp_capabilities`); P5.7 added the second
//    (`unreal_open_mcp_bridge_status`). P8.6 folded both local handlers and the
//    live dispatch into a single `ToolRouter` (`tool-router.ts`) — the dispatch
//    spine this file delegates `tools/call` to. The router owns the policy
//    table (live / offline / local / batch), resolves local tools in-process,
//    POSTs live tools through the LiveClient, and stamps `_source` + `_route`
//    metadata on every JSON result. Offline handlers (disk parsers) and batch
//    (headless commandlet) are recognized policies that refuse with structured
//    `offline_not_implemented` / `batch_not_implemented` until their phases
//    land; they never silently fall through to live. The pre-P8.6 local
//    dispatch (`handleLocalTool` + the inline `LOCAL_TOOLS` branching) is gone;
//    `handleLocalTool` survives as a thin test facade that builds a throwaway
//    `ToolRouter` so the bridge_status unit tests keep driving the local path
//    without booting the live transport.
//  - Handlers (`handleListTools`, `handleCallTool`) are exported standalone so
//    tests can exercise them directly. Unity inlines them inside `createServer`
//    and tests other modules; we export them so the dispatch + fallback paths
//    are unit-testable without booting stdio. The dispatch router is a module-
//    level holder with a setter so `main()` installs the real router while
//    tests can install a stub transport or clear it via
//    `resetLiveRouterForTest`.
//  - Explicit `transport.onclose` → `server.close()` → `process.exit(0)` to
//    make the "clean exit on disconnect" contract observable rather than
//    relying solely on the event loop draining.
