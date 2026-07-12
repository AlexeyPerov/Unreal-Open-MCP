#!/usr/bin/env node

// stdio MCP server scaffold for Unreal Open MCP.
//
// P1.5 scope: boot a Model Context Protocol server over stdio, register the
// tools/list and tools/call handlers against an (intentionally empty) tool
// registry, and exit cleanly on disconnect. There is no bridge routing,
// instance discovery, resource surface, or CLI dispatch in this task — those
// land in later phases. The server is the stdio hop an AI client connects to;
// it must boot and answer an empty tools/list before the first real tool
// (`unreal_open_mcp_ping`) is wired in.
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
 * agent can self-correct. No tools are registered in P1.5, so every call is an
 * unknown-tool response until the registry is filled. Exported for direct unit
 * tests.
 */
export async function handleCallTool(
  request: CallToolRequest,
): Promise<CallToolResult> {
  const { name } = request.params;
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
  // Handler routing lands in a later phase. The registry is empty in P1.5 so
  // this branch is unreachable today; the marker keeps the contract honest.
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
 * missing. Port discovery is out of scope for P1.5 (the registry is empty);
 * the resolved project path is logged so users can confirm the binding.
 */
function getEnv(): { projectPath: string } {
  const projectPath = process.env[PROJECT_PATH_ENV_VAR];
  if (!projectPath) {
    console.error(
      `${SERVER_NAME}: ${PROJECT_PATH_ENV_VAR} environment variable is required.`,
    );
    process.exit(1);
  }
  console.error(`[${SERVER_NAME}] Bound to project: ${projectPath}`);
  return { projectPath };
}

async function main(): Promise<void> {
  getEnv();
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
//  - No port resolution / instance discovery / auth token in P1.5 — the
//    registry is empty; discovery lands in a later phase.
//  - No LiveClient / BatchSpawn / ToolRouter / resources / CLI dispatch.
//  - Handlers (`handleListTools`, `handleCallTool`) are exported standalone so
//    tests can exercise them directly. Unity inlines them inside `createServer`
//    and tests other modules; we export them because the registry is empty and
//    these are the only meaningful units to test in P1.5.
//  - Explicit `transport.onclose` → `server.close()` → `process.exit(0)` to
//    make the "clean exit on disconnect" contract observable rather than
//    relying solely on the event loop draining.
