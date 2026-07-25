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
import { buildCapabilities } from "./capabilities/build-capabilities.js";
import { RULE_CATALOG, FIX_CATALOG } from "./capabilities/rule-catalog.js";
import { readPackageVersion } from "./package-version.js";
import {
  computePort,
  isUsablePort,
  authTokenFromLock,
  PORT_OVERRIDE_ENV_VAR,
  readInstanceLock,
  isPidAlive,
  classifyInstance,
  lockPath,
  type InstanceLock,
} from "./instance-discovery.js";
import { LiveClient, type Router } from "./live-client.js";
import {
  deriveBridgeStatus,
  summarizeInstanceLock,
  bridgeStatusRecoveryHint,
  bridgeStatusNextStep,
  type PingProbe,
} from "./tools/bridge-status-helpers.js";

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
 * Local-route tools — resolved in-process by {@link handleLocalTool} without a
 * bridge POST round-trip. `capabilities` (P3.8) builds the capability surface
 * from the registered tool list + the static rule/fix catalog, so it works with
 * the editor down. `bridge_status` (P5.7) composes the instance-lock classifier
 * with one /ping probe (fired through the live router when one is installed); a
 * dead/stopped bridge is a successful status read, not an error. Adding a
 * local-route tool means extending this set AND adding a handler branch in
 * {@link handleLocalTool}.
 */
const LOCAL_TOOLS: ReadonlySet<string> = new Set([
  "unreal_open_mcp_capabilities",
  "unreal_open_mcp_bridge_status",
]);

/**
 * Resolve a local-route tool call in-process. Returns `null` when the tool is
 * not a local-route tool (the caller then routes it through the live bridge).
 *
 * `router` is the currently-installed live router (the same one `handleCallTool`
 * dispatches live tools through). It is optional and threaded in only so
 * `bridge_status` can fire its /ping probe through the installed router;
 * capabilities ignores it. When null, bridge_status treats the ping as failed
 * (it still reports a status from the lock alone). Exported so unit tests can
 * drive the local dispatch directly without booting the live router.
 */
export async function handleLocalTool(
  name: string,
  args: Record<string, unknown>,
  router: Router | null = null,
): Promise<CallToolResult | null> {
  if (!LOCAL_TOOLS.has(name)) {
    return null;
  }
  if (name === "unreal_open_mcp_capabilities") {
    const kind =
      args.kind === "tools" || args.kind === "rules" || args.kind === "fixes"
        ? (args.kind as "tools" | "rules" | "fixes")
        : undefined;
    const includePlanned = args.include_planned !== false;
    const result = buildCapabilities(
      {
        tools: ALL_TOOLS,
        rules: RULE_CATALOG,
        fixes: FIX_CATALOG,
      },
      { kind, includePlanned },
    );
    return {
      content: [{ type: "text", text: JSON.stringify(result) }],
      isError: false,
    };
  }
  if (name === "unreal_open_mcp_bridge_status") {
    return resolveBridgeStatus(router);
  }
  // Unreachable: LOCAL_TOOLS membership is checked above and every member has
  // a handler branch. Defensive fallback keeps the call honest.
  return {
    isError: true,
    content: [
      {
        type: "text",
        text: `Tool ${name} is registered as local-route but has no in-process handler.`,
      },
    ],
  };
}

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
 * The Unreal project path the server is bound to, set by {@link getEnv} during
 * `main()`. Exported so unit tests can point it at a temp dir to plant/read an
 * instance lock without going through `main()`.
 */
let boundProjectPath: string | null = null;

/**
 * Set the bound project path used by {@link resolveBridgeStatus} to read the
 * instance lock. Installed by `main()` after env resolution; exported so tests
 * can drive bridge_status against a temp project without booting stdio.
 */
export function setBoundProjectPath(projectPath: string | null): void {
  boundProjectPath = projectPath;
}

/**
 * Resolve `unreal_open_mcp_bridge_status` in-process. Composes the instance-lock
 * classifier with one /ping probe fired through the installed `router`, then
 * maps the result with the pure `deriveBridgeStatus`. A dead / stopped bridge is
 * a *successful* status read — this never returns `isError:true`.
 *
 * The project path comes from {@link boundProjectPath} (set by `main()`); when
 * unset (a unit test that drives the handler directly), no lock is read and the
 * lock-derived signals are absent (classification defaults to `gone`).
 */
async function resolveBridgeStatus(
  router: Router | null,
): Promise<CallToolResult> {
  const projectPath = boundProjectPath;
  const lock: InstanceLock | null = projectPath
    ? readInstanceLock(projectPath)
    : null;
  const classification = classifyInstance(lock);

  // Fire one /ping probe through the installed router. When no router is
  // installed (pre-main wiring / a direct unit test), the probe is treated as
  // failed — the status is then derived from the lock alone (stopped unless a
  // stale-heartbeat lock classifies dead_bridge).
  const ping = await probeBridge(router);

  const status = deriveBridgeStatus({ classification, ping });

  const body = {
    status,
    ready: status === "running",
    projectPath: projectPath ?? null,
    classification,
    recoveryHint: bridgeStatusRecoveryHint(status),
    instance: {
      lockPath: projectPath ? lockPath(projectPath) : null,
      classification,
      lock: summarizeInstanceLock(lock),
    },
    ping:
      ping !== null && ping.kind === "ok"
        ? {
            reachable: true,
            connected: ping.connected,
            compiling: ping.compiling ?? null,
            isPlaying: ping.isPlaying ?? null,
            unrealVersion: ping.unrealVersion ?? null,
            bridgeVersion: ping.bridgeVersion ?? null,
            mode: ping.mode ?? null,
          }
        : { reachable: false },
    nextStep: bridgeStatusNextStep(status),
  };

  return {
    content: [{ type: "text", text: JSON.stringify(body) }],
    isError: false,
  };
}

/**
 * Fire one /ping probe through the router and parse the body into a
 * {@link PingProbe}. Returns `{ kind: "fail" }` when the router is null, the
 * probe returned an error, or the body did not parse — the status mapper only
 * needs reachable-vs-fail plus the health fields on success.
 */
async function probeBridge(router: Router | null): Promise<PingProbe> {
  if (!router) return { kind: "fail" };
  let result: CallToolResult;
  try {
    result = await router.route("unreal_open_mcp_ping", {});
  } catch {
    return { kind: "fail" };
  }
  if (result.isError) return { kind: "fail" };
  const first = result.content[0];
  if (!first || first.type !== "text" || typeof first.text !== "string") {
    return { kind: "fail" };
  }
  let parsed: Record<string, unknown>;
  try {
    parsed = JSON.parse(first.text);
  } catch {
    return { kind: "fail" };
  }
  if (!parsed || typeof parsed !== "object") return { kind: "fail" };
  return {
    kind: "ok",
    connected: parsed.connected === true,
    compiling:
      typeof parsed.compiling === "boolean" ? parsed.compiling : undefined,
    isPlaying:
      typeof parsed.isPlaying === "boolean" ? parsed.isPlaying : undefined,
    unrealVersion:
      typeof parsed.unrealVersion === "string" ? parsed.unrealVersion : null,
    bridgeVersion:
      typeof parsed.bridgeVersion === "string" ? parsed.bridgeVersion : undefined,
    mode: typeof parsed.mode === "string" ? parsed.mode : undefined,
  };
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
  // Local-route tools resolve in-process — no bridge POST round-trip. The
  // capabilities tool (P3.8) works with the editor down because the rule/fix
  // catalog is static and the tool list is in-memory. bridge_status (P5.7)
  // composes the lock classifier + one /ping probe through the installed
  // router; a dead/stopped bridge is a successful status read.
  // Resolve before the live-router check so a missing router never blocks a
  // local tool (and so a unit test with no router installed can still call
  // capabilities directly).
  const localResult = await handleLocalTool(name, args ?? {}, liveRouter);
  if (localResult !== null) {
    return localResult;
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
  // LiveClient so the tools/call handler can dispatch live-routed tools
  // (`unreal_open_mcp_ping` is the first; other tools land in later phases).
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
//    other names). No BatchSpawn / ToolRouter / offline routing / resources /
//    CLI dispatch yet. P3.8 adds the first **local-route** tool
//    (`unreal_open_mcp_capabilities`) — resolved in-process by
//    `handleLocalTool` before the live-router check, so it works with the
//    editor down. P5.7 adds the second local-route tool
//    (`unreal_open_mcp_bridge_status`) — composes the lock classifier with one
//    /ping probe fired through the installed router; `handleLocalTool` now
//    receives the router so the probe reuses the same LiveClient path. The full
//    per-tool router (live / offline / local / batch) lands in Phase 8 and will
//    absorb this dispatch into a `ToolRouter`.
//  - Handlers (`handleListTools`, `handleCallTool`) are exported standalone so
//    tests can exercise them directly. Unity inlines them inside `createServer`
//    and tests other modules; we export them so the dispatch + fallback paths
//    are unit-testable without booting stdio. The live router is a module-level
//    holder with a setter so `main()` installs the real client while tests can
//    install a stub or clear it via `resetLiveRouterForTest`.
//  - Explicit `transport.onclose` → `server.close()` → `process.exit(0)` to
//    make the "clean exit on disconnect" contract observable rather than
//    relying solely on the event loop draining.
