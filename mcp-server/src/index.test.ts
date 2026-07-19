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
} from "./index.js";
import type { CallToolResult } from "@modelcontextprotocol/sdk/types.js";

const here = dirname(fileURLToPath(import.meta.url));
// dist-test layout: dist-test/index.js + dist-test/index.test.js. The compiled
// server entrypoint under test is the sibling dist-test/index.js.
const SERVER_ENTRY = resolve(here, "index.js");

// tools/list returns the registered tool set. P1.7 registered the first tool
// (`unreal_open_mcp_ping`); P2.2 added `unreal_open_mcp_actor_find`; P2.3 added
// `unreal_open_mcp_actor_create`; P2.4 added `unreal_open_mcp_actor_modify` +
// `unreal_open_mcp_object_modify`; P2.5 added actor_set_parent /
// actor_duplicate / actor_destroy + the five actor_component_* tools; P2.6
// added the five level lifecycle tools (level_open / level_save /
// level_list_loaded / level_set_current / level_unload_sublevel); P2.7 added
// the level inspect + create pair (level_get_data / level_create); P3.6 added
// the three gate meta-tools (validate_edit / checkpoint_create / delta); P3.7
// added apply_fix; P3.8 added capabilities; P4.1 added the asset read pair
// (asset_find / asset_get_data). Further tools land in later phases and
// append here.
test("handleListTools returns the registered tools", async () => {
  const result = await handleListTools();
  assert.equal(result.tools.length, 27);
  assert.equal(result.tools[0].name, "unreal_open_mcp_ping");
  assert.equal(result.tools[1].name, "unreal_open_mcp_actor_find");
  assert.equal(result.tools[2].name, "unreal_open_mcp_actor_create");
  assert.equal(result.tools[3].name, "unreal_open_mcp_actor_modify");
  assert.equal(result.tools[4].name, "unreal_open_mcp_object_modify");
  assert.equal(result.tools[5].name, "unreal_open_mcp_actor_set_parent");
  assert.equal(result.tools[6].name, "unreal_open_mcp_actor_duplicate");
  assert.equal(result.tools[7].name, "unreal_open_mcp_actor_destroy");
  assert.equal(result.tools[8].name, "unreal_open_mcp_actor_component_add");
  assert.equal(result.tools[9].name, "unreal_open_mcp_actor_component_destroy");
  assert.equal(result.tools[10].name, "unreal_open_mcp_actor_component_get");
  assert.equal(result.tools[11].name, "unreal_open_mcp_actor_component_modify");
  assert.equal(result.tools[12].name, "unreal_open_mcp_actor_component_list_all");
});

// Unknown tool → structured MCP error with isError, listing registered names.
// The suffix names every registered tool so the agent can self-correct.
test("handleCallTool returns isError for an unknown tool", async () => {
  // Clear any router a previous test installed so this case is isolated.
  resetLiveRouterForTest();
  const result = await handleCallTool({
    params: { name: "unreal_open_mcp_does_not_exist", arguments: {} },
  } as unknown as Parameters<typeof handleCallTool>[0]);
  assert.equal(result.isError, true);
  assert.ok(Array.isArray(result.content));
  const text = (result.content[0] as { type: string; text: string }).text;
  assert.match(text, /Unknown tool:/);
  assert.match(text, /Registered tools: unreal_open_mcp_ping, unreal_open_mcp_actor_find, unreal_open_mcp_actor_create, unreal_open_mcp_actor_modify, unreal_open_mcp_object_modify/);
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
// handleCallTool → LiveClient dispatch wiring without booting stdio.
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
    assert.equal(result, stubResult);
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
  assert.equal(payload.counts.toolsImplemented, 27);
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
  assert.equal(tools.length, 27);
  assert.equal(tools[0].name, "unreal_open_mcp_ping");
  assert.equal(tools[1].name, "unreal_open_mcp_actor_find");
  assert.equal(tools[2].name, "unreal_open_mcp_actor_create");
  assert.equal(tools[3].name, "unreal_open_mcp_actor_modify");
  assert.equal(tools[4].name, "unreal_open_mcp_object_modify");
  assert.equal(tools[5].name, "unreal_open_mcp_actor_set_parent");
  assert.equal(tools[6].name, "unreal_open_mcp_actor_duplicate");
  assert.equal(tools[7].name, "unreal_open_mcp_actor_destroy");
  assert.equal(tools[8].name, "unreal_open_mcp_actor_component_add");
  assert.equal(tools[9].name, "unreal_open_mcp_actor_component_destroy");
  assert.equal(tools[10].name, "unreal_open_mcp_actor_component_get");
  assert.equal(tools[11].name, "unreal_open_mcp_actor_component_modify");
  assert.equal(tools[12].name, "unreal_open_mcp_actor_component_list_all");
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
