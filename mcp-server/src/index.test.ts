import test from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

import {
  createServer,
  handleCallTool,
  handleListTools,
  setLiveRouter,
  resetLiveRouterForTest,
  SERVER_NAME,
  PROJECT_PATH_ENV_VAR,
  sessionState,
} from "./index.js";
import type { CallToolResult } from "@modelcontextprotocol/sdk/types.js";

const here = dirname(fileURLToPath(import.meta.url));
// dist-test layout: dist-test/index.js + dist-test/index.test.js. The compiled
// server entrypoint under test is the sibling dist-test/index.js.
const SERVER_ENTRY = resolve(here, "index.js");

// tools/list returns the *visible* tool set, filtered through the per-session
// tool-group state (P8.9). A fresh session advertises the lean `core` surface
// plus the always-visible meta / recovery tools. The full registry is larger
// (71 tools as of P8.10); the default surface is intentionally small so an
// agent's prompt is not bloated before it activates a group.
//
// Registry history (the full set is larger than the default surface): P1.7
// registered `unreal_open_mcp_ping`; P2.x added the actor / object / level
// families; P3.6 added the three gate meta-tools; P3.7 added apply_fix; P3.8
// added capabilities; P4.x added asset / material / import; P5.x added editor /
// selection / console / reflection / screenshot / bridge_status; P6.x added
// the Blueprint family; P7.x added the source family; P8.7 added the offline
// readers. Further tools land in later phases.
test("handleListTools returns the lean default surface for a fresh session", async () => {
  // Isolate: a previous test may have activated a group on the shared session
  // state. Reset to defaults so this assertion pins the fresh-session surface.
  sessionState.reset();
  const result = await handleListTools();
  // Default-on group is `core` only (ping) + the always-visible meta / recovery
  // tools. ping is both in `core` and always-visible, so it is counted once.
  // The offline recovery tools (source_read_offline, project_index) have no
  // group assignment (null group) and are always visible via the filter's null
  // fallback.
  const names = result.tools.map((t) => t.name).sort();
  assert.deepEqual(names, [
    "unreal_open_mcp_bridge_status",
    "unreal_open_mcp_capabilities",
    "unreal_open_mcp_manage_tools",
    "unreal_open_mcp_ping",
    "unreal_open_mcp_project_index",
    "unreal_open_mcp_read_compile_errors",
    "unreal_open_mcp_source_read_offline",
  ]);
});

test("handleListTools reflects the full registry when every group is active", async () => {
  // Sanity check the filter is not accidentally dropping tools when nothing is
  // hidden. Activate every group and confirm the surface matches the registry
  // size (the non-grouped meta / recovery tools are always visible, so the
  // union is the full registry).
  sessionState.reset();
  sessionState.activate("gate-and-verify");
  sessionState.activate("typed-editor");
  try {
    const result = await handleListTools();
    assert.equal(result.tools.length, 71);
    assert.equal(result.tools[0].name, "unreal_open_mcp_ping");
  } finally {
    sessionState.reset();
  }
});

// Unknown tool → structured MCP error with isError, listing the *visible*
// registered names (the session-filtered set the agent sees via tools/list).
test("handleCallTool returns isError for an unknown tool", async () => {
  // Clear any router a previous test installed so this case is isolated.
  resetLiveRouterForTest();
  sessionState.reset();
  const result = await handleCallTool({
    params: { name: "unreal_open_mcp_does_not_exist", arguments: {} },
  } as unknown as Parameters<typeof handleCallTool>[0]);
  assert.equal(result.isError, true);
  assert.ok(Array.isArray(result.content));
  const text = (result.content[0] as { type: string; text: string }).text;
  assert.match(text, /Unknown tool:/);
  // The self-correction suffix names the visible tools (the lean default
  // surface), not the full registry.
  assert.match(text, /unreal_open_mcp_ping/);
  assert.doesNotMatch(
    text,
    /unreal_open_mcp_actor_find/,
    "a hidden tool must not be listed as reachable",
  );
});

// A known tool with no live router installed falls back to a "not wired" error
// instead of crashing. This is the scaffold path (unit tests / pre-main wiring).
test("handleCallTool returns a not-wired error for a known tool when no router is installed", async () => {
  resetLiveRouterForTest();
  const result = await handleCallTool({
    params: { name: "unreal_open_mcp_ping", arguments: {} },
  } as unknown as Parameters<typeof handleCallTool>[0]);
  assert.equal(result.isError, true);
  const text = (result.content[0] as { type: string; text: string }).text;
  assert.match(text, /no handler wired/i);
});

// A known tool with a live router installed is routed through it. Proves the
// handleCallTool → ToolRouter → LiveClient dispatch wiring without booting
// stdio. The ToolRouter stamps `_source` + `_route` metadata on the JSON body
// (P8.6), so the assertion checks the inner payload survives verbatim AND the
// route metadata identifies the live path.
test("handleCallTool dispatches a known tool through the installed live router", async () => {
  const stubResult: CallToolResult = {
    content: [{ type: "text", text: '{"connected":true}' }],
    isError: false,
  };
  const routed: string[] = [];
  setLiveRouter({
    async route(name: string, args: Record<string, unknown>) {
      routed.push(name);
      assert.deepEqual(args, {});
      return stubResult;
    },
  });
  try {
    const result = await handleCallTool({
      params: { name: "unreal_open_mcp_ping", arguments: {} },
    } as unknown as Parameters<typeof handleCallTool>[0]);
    assert.deepEqual(routed, ["unreal_open_mcp_ping"]);
    assert.equal(result.isError, false);
    const body = JSON.parse(
      (result.content[0] as { type: string; text: string }).text,
    ) as { connected: boolean; _source: string; _route: { route: string } };
    assert.equal(body.connected, true);
    assert.equal(body._source, "live");
    assert.equal(body._route.route, "live");
  } finally {
    resetLiveRouterForTest();
  }
});

// P3.8 — capabilities is local-route: it resolves in-process before the live
// router is consulted, so it works with the editor down (no router installed)
// AND never touches the router when one is installed. Both branches are pinned
// here so a later refactor cannot accidentally route capabilities live.
test("handleCallTool resolves unreal_open_mcp_capabilities locally without a router", async () => {
  resetLiveRouterForTest();
  const result = await handleCallTool({
    params: { name: "unreal_open_mcp_capabilities", arguments: {} },
  } as unknown as Parameters<typeof handleCallTool>[0]);
  assert.equal(result.isError, false);
  const text = (result.content[0] as { type: string; text: string }).text;
  const payload = JSON.parse(text) as {
    tools: unknown[];
    rules: unknown[];
    fixes: unknown[];
    counts: { toolsImplemented: number; rulesImplemented: number };
  };
  assert.ok(payload.tools.length > 0);
  assert.ok(payload.rules.length > 0);
  assert.ok(payload.fixes.length > 0);
  // Every registered tool is surfaced (capabilities is itself included).
  assert.equal(payload.counts.toolsImplemented, 71);
  assert.equal(payload.counts.rulesImplemented, 3);
});

test("handleCallTool does not consult the live router for unreal_open_mcp_capabilities", async () => {
  const routed: string[] = [];
  setLiveRouter({
    async route(name: string) {
      routed.push(name);
      return { content: [{ type: "text", text: "" }], isError: false };
    },
  });
  try {
    await handleCallTool({
      params: { name: "unreal_open_mcp_capabilities", arguments: { kind: "rules" } },
    } as unknown as Parameters<typeof handleCallTool>[0]);
    assert.deepEqual(routed, [], "capabilities must NOT route through the bridge");
  } finally {
    resetLiveRouterForTest();
  }
});

test("handleCallTool honors kind=rules filter on capabilities", async () => {
  resetLiveRouterForTest();
  const result = await handleCallTool({
    params: { name: "unreal_open_mcp_capabilities", arguments: { kind: "rules" } },
  } as unknown as Parameters<typeof handleCallTool>[0]);
  assert.equal(result.isError, false);
  const payload = JSON.parse(
    (result.content[0] as { type: string; text: string }).text,
  ) as { tools: unknown[]; rules: unknown[]; fixes: unknown[] };
  assert.equal(payload.tools.length, 0);
  assert.ok(payload.rules.length > 0);
  assert.equal(payload.fixes.length, 0);
});

// createServer wires the handlers without booting stdio. The MCP initialize
// handshake name is the published server identity.
test("createServer returns a Server with the published name", async () => {
  const server = createServer();
  // The Server exposes its name/version via the private `_serverInfo`; we
  // assert the wiring indirectly by confirming it constructed and has the
  // request handlers registered. The handshake is exercised end-to-end in the
  // subprocess test below.
  assert.ok(server);
  assert.equal(typeof server.setRequestHandler, "function");
  await server.close();
});

// --- subprocess / lifecycle tests -----------------------------------------

/**
 * Spawn the compiled entrypoint and drive a minimal MCP initialize →
 * tools/list → EOF handshake over stdio. Confirms:
 *  - the process boots with UNREAL_PROJECT_PATH set,
 *  - initialize reports the published server name,
 *  - tools/list returns the registered tool set (ping + actor_find),
 *  - the process exits 0 after stdin EOF (clean disconnect).
 */
test("subprocess: boots, answers initialize + tools/list, exits 0 on EOF", async () => {
  const child = spawn(process.execPath, [SERVER_ENTRY], {
    env: { ...process.env, [PROJECT_PATH_ENV_VAR]: "/tmp/FakeUnrealProject" },
    stdio: ["pipe", "pipe", "pipe"],
  });

  let stdout = "";
  let stderr = "";
  child.stdout.on("data", (chunk: Buffer) => (stdout += chunk.toString()));
  child.stderr.on("data", (chunk: Buffer) => (stderr += chunk.toString()));

  const send = (obj: unknown) =>
    child.stdin.write(`${JSON.stringify(obj)}\n`);

  // MCP initialize handshake.
  send({
    jsonrpc: "2.0",
    id: 1,
    method: "initialize",
    params: {
      protocolVersion: "2024-11-05",
      capabilities: {},
      clientInfo: { name: "test-client", version: "0.0.0" },
    },
  });
  // notifications/initialized — sent after initialize per the spec.
  send({ jsonrpc: "2.0", method: "notifications/initialized" });
  send({ jsonrpc: "2.0", id: 2, method: "tools/list" });

  // Give the server a beat to answer, then EOF stdin to disconnect.
  await new Promise((r) => setTimeout(r, 300));
  child.stdin.end();

  const code = await new Promise<number>((res, rej) => {
    child.on("error", rej);
    child.on("exit", (c) => res(c ?? -1));
  });

  assert.equal(code, 0, `unexpected exit code. stderr:\n${stderr}`);

  const messages = stdout
    .split("\n")
    .filter((line) => line.trim().length > 0)
    .map((line) => JSON.parse(line) as Record<string, unknown>);

  const init = messages.find((m) => m.id === 1) as
    | { result?: { serverInfo?: { name?: string } } }
    | undefined;
  assert.ok(init, "initialize response missing");
  assert.equal(init?.result?.serverInfo?.name, SERVER_NAME);

  const list = messages.find((m) => m.id === 2) as
    | { result?: { tools?: Array<{ name: string }> } }
    | undefined;
  assert.ok(list, "tools/list response missing");
  const tools = list?.result?.tools ?? [];
  // A fresh subprocess session advertises the lean default surface: the `core`
  // group (ping) plus the always-visible meta / recovery tools (capabilities,
  // bridge_status, read_compile_errors, source_read_offline, project_index —
  // the last two have a null group and survive the filter's null fallback).
  // The full registry is larger; groups activate on demand via manage_tools.
  const names = tools.map((t) => t.name).sort();
  assert.deepEqual(names, [
    "unreal_open_mcp_bridge_status",
    "unreal_open_mcp_capabilities",
    "unreal_open_mcp_manage_tools",
    "unreal_open_mcp_ping",
    "unreal_open_mcp_project_index",
    "unreal_open_mcp_read_compile_errors",
    "unreal_open_mcp_source_read_offline",
  ]);
});

// Missing UNREAL_PROJECT_PATH → exit 1 with a clear stderr message.
test("subprocess: exits 1 when UNREAL_PROJECT_PATH is missing", async () => {
  const env = { ...process.env };
  delete env[PROJECT_PATH_ENV_VAR];
  const child = spawn(process.execPath, [SERVER_ENTRY], {
    env,
    stdio: ["pipe", "pipe", "pipe"],
  });

  let stderr = "";
  child.stderr.on("data", (chunk: Buffer) => (stderr += chunk.toString()));

  const code = await new Promise<number>((res, rej) => {
    child.on("error", rej);
    child.on("exit", (c) => res(c ?? -1));
  });

  assert.equal(code, 1, `unexpected exit code. stderr:\n${stderr}`);
  assert.match(stderr, new RegExp(PROJECT_PATH_ENV_VAR));
});
