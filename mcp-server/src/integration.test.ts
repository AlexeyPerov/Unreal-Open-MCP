// P1.9 integration tests — the phase-gate parity smoke (in-process).
//
// These exercise the FULL stdio-route path a real MCP client uses, end to end:
//   MCP SDK Client  ⇄  InMemoryTransport  ⇄  createServer()  →  handleCallTool
//   →  LiveClient  →  GET /ping on a real loopback HTTP stub.
//
// Where the live-client unit tests (live-client.test.ts) drive LiveClient in
// isolation, these tests prove the *wiring*: that tools/list advertises ping,
// that a tools/call dispatches through the installed live router into the
// LiveClient, and that the PingResponse body / failure envelopes survive the
// MCP round-trip intact. Three outcomes are pinned — healthy, bridge-down, and
// HTTP 500 — because they are the three the acceptance criteria and the
// failure-signature cheat sheet call out as load-bearing for Phase 2 entry.
//
// A scripted subprocess smoke (scripts/p1-parity-smoke.mjs, `npm run smoke:p1`)
// complements this file by exercising the BUILT dist/index.js artifact over
// stdio — that one guards packaging/transport drift this in-process suite does
// not see.
//
// Adapted from Unity Open MCP's mcp-server/src/integration.test.ts (adapt
// fidelity, P1.9). Unity's integration tests cover the resources/router layers
// that do not exist yet here; this port narrows to the ping route that P1.7
// shipped, which is exactly the Phase 1 exit gate.
//
// P2.8 extended this file with the Phase 2 exit-gate smoke: the first typed
// tool, unreal_open_mcp_actor_find, round-tripped through the full POST
// /tools/{name} dispatch with the {ok,result,error} envelope. The P2.8 cases
// live in the second half of the file and pin healthy envelope unwrap,
// bridge-down inheritance of bridge_offline, and the {ok:false,error} tool
// failure envelope — the three outcomes the Phase 2 acceptance criteria call
// out as load-bearing before Phase 3 (gate/verify) can start.

import { test } from "node:test";
import assert from "node:assert/strict";
import {
  createServer,
  type Server as HttpServer,
  type IncomingMessage,
  type ServerResponse,
} from "node:http";
import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";

import {
  createServer as createMcpServer,
  setLiveRouter,
  resetLiveRouterForTest,
  SERVER_NAME,
} from "./index.js";
import { LiveClient, type PingResponse } from "./live-client.js";

/** Canonical 200 /ping body the Unreal bridge emits (pinned field set). */
const HEALTHY_PING: PingResponse = {
  connected: true,
  status: "ready",
  projectPath: "/tmp/test-project",
  unrealVersion: "5.8.0",
  bridgeVersion: "0.0.1",
  mode: "live",
  port: 21111,
  compiling: false,
  isPlaying: false,
};

interface BridgeStub {
  server: HttpServer;
  port: number;
  close(): Promise<void>;
}

/** Start an ephemeral loopback HTTP "bridge" that answers /ping with the body. */
function startBridgeStub(body: PingResponse): Promise<BridgeStub> {
  return new Promise((resolve) => {
    const server = createServer((_req, res) => {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify(body));
    });
    server.listen(0, "127.0.0.1", () => {
      const addr = server.address();
      const port = typeof addr === "object" && addr ? addr.port : 0;
      resolve({
        server,
        port,
        close: () => new Promise<void>((r) => server.close(() => r())),
      });
    });
  });
}

/** Start a loopback HTTP "bridge" that answers /ping with a 500 error body. */
function startErrorBridgeStub(
  status: number,
  body: unknown,
): Promise<BridgeStub> {
  return new Promise((resolve) => {
    const server = createServer((_req, res) => {
      res.writeHead(status, { "Content-Type": "application/json" });
      res.end(JSON.stringify(body));
    });
    server.listen(0, "127.0.0.1", () => {
      const addr = server.address();
      const port = typeof addr === "object" && addr ? addr.port : 0;
      resolve({
        server,
        port,
        close: () => new Promise<void>((r) => server.close(() => r())),
      });
    });
  });
}

/**
 * Start an ephemeral loopback HTTP "bridge" with a custom handler so a test
 * can dispatch by method + URL. P1's startBridgeStub / startErrorBridgeStub
 * ignore the method and answer every path with the same body — fine for the
 * /ping-only coverage but unable to exercise the POST /tools/{name} envelope
 * round-trip that P2.8 pins. This stub mirrors the setHandler pattern from
 * live-client.test.ts.
 */
function startHandlerStub(
  handler: (req: IncomingMessage, res: ServerResponse) => void,
): Promise<BridgeStub> {
  return new Promise((resolve) => {
    const server = createServer((req, res) => handler(req, res));
    server.listen(0, "127.0.0.1", () => {
      const addr = server.address();
      const port = typeof addr === "object" && addr ? addr.port : 0;
      resolve({
        server,
        port,
        close: () => new Promise<void>((r) => server.close(() => r())),
      });
    });
  });
}

/** Drain a request body to a string. POST /tools/{name} always carries a JSON
 *  args body — reading it keeps the stub HTTP-compliant (the client may retry
 *  or hold the socket if the body is never consumed). */
function readBody(req: IncomingMessage): Promise<string> {
  return new Promise((resolve) => {
    let data = "";
    req.on("data", (chunk) => (data += chunk.toString()));
    req.on("end", () => resolve(data));
  });
}

/**
 * Wire a real MCP SDK Client to `createServer()` over an in-memory transport
 * pair, with the live router pointed at a LiveClient for the given bridge port.
 * Returns the client + a cleanup that tears both ends down and clears the
 * module-level router so cases are isolated.
 */
async function setupClient(port: number, authToken?: string): Promise<{
  client: Client;
  cleanup: () => Promise<void>;
}> {
  setLiveRouter(new LiveClient(port, authToken));
  const server: Server = createMcpServer();
  const [clientTransport, serverTransport] =
    InMemoryTransport.createLinkedPair();
  await server.connect(serverTransport);

  const client = new Client(
    { name: "integration-test-client", version: "0.0.0" },
    { capabilities: {} },
  );
  await client.connect(clientTransport);

  return {
    client,
    cleanup: async () => {
      await client.close();
      await server.close();
      resetLiveRouterForTest();
    },
  };
}

/**
 * Parse the first text content block of a CallToolResult as JSON. The SDK's
 * `client.callTool` return type is a union (a content-bearing branch plus a
 * task-stream branch with `toolResult`); we narrow to the content branch at
 * runtime, so the parameter is `unknown` and asserted before use.
 */
function bodyOf(result: unknown): unknown {
  const r = result as { content?: Array<{ type: string; text?: string }> };
  const block = r.content?.[0];
  assert.ok(block?.type === "text", "first content block must be text");
  assert.ok(typeof block.text === "string", "text block must carry a string");
  return JSON.parse(block.text as string);
}

/**
 * Strip the `_source` / `_route` metadata the ToolRouter stamps onto every
 * JSON result (P8.6), so a parity-pin `deepEqual` against the inner tool body
 * is not polluted by the routing envelope. Use {@link bodyOf} when a test
 * wants to inspect the metadata itself.
 */
function payloadOf(result: unknown): Record<string, unknown> {
  const body = bodyOf(result) as Record<string, unknown>;
  const { _source: _s, _route: _r, ...payload } = body;
  void _s;
  void _r;
  return payload;
}

// --- tools/list advertises ping over the MCP wire ---------------------------

test("integration: tools/list advertises unreal_open_mcp_ping", async () => {
  // No bridge needed for listing — but the router must still be wired so a
  // stray tools/call couldn't crash. Point it at a dead port; we never call.
  const { client, cleanup } = await setupClient(1);
  try {
    const { tools } = await client.listTools();
    const names = tools.map((t) => t.name);
    assert.ok(
      names.includes("unreal_open_mcp_ping"),
      `ping must be advertised; got ${names.join(", ")}`,
    );
    // P2 registry is ping + the actor/object tools landed so far (P2.2 added
    // actor_find; P2.3 added actor_create; P2.4 added actor_modify +
    // object_modify; P2.5 added actor_set_parent / actor_duplicate /
    // actor_destroy + the five actor_component_* tools; P2.6 added the five
    // level lifecycle tools — level_open / level_save / level_list_loaded /
    // level_set_current / level_unload_sublevel; P2.7 added the level
    // inspect + create pair — level_get_data / level_create; P3.6 added the
    // three gate meta-tools — validate_edit / checkpoint_create / delta; P3.7
    // added apply_fix; P3.8 added capabilities; P4.1 added the asset read
    // pair — asset_find / asset_get_data; P4.2 added the Content Browser
    // CRUD family — asset_create_folder / asset_copy / asset_move /
    // asset_delete / asset_refresh; P4.3 added the material family —
    // material_create / material_modify / material_get_data; P5.1 added the
    // editor family; P5.2 added editor selection; P5.3 added the console
    // family; P5.4 added the reflection family; P5.5 added the screenshot
    // family; P5.7 added bridge_status; P6.1 added the Blueprint family —
    // blueprint_create / blueprint_get; P6.2 added the Blueprint SCS component
    // pair — blueprint_add_component / blueprint_remove_component; P6.3 added
    // the Blueprint variable family — blueprint_add_variable /
    // blueprint_modify_variable / blueprint_set_default; P6.4 added the
    // Blueprint function/event stub pair — blueprint_add_function /
    // blueprint_add_event; P6.5 added blueprint_compile; P6.6 added
    // blueprint_spawn). Guard against accidental registry drift silently
    // changing what the phase-gate smoke covers.
    assert.deepEqual(names, [
      "unreal_open_mcp_ping",
      "unreal_open_mcp_actor_find",
      "unreal_open_mcp_actor_create",
      "unreal_open_mcp_actor_modify",
      "unreal_open_mcp_object_modify",
      "unreal_open_mcp_actor_set_parent",
      "unreal_open_mcp_actor_duplicate",
      "unreal_open_mcp_actor_destroy",
      "unreal_open_mcp_actor_component_add",
      "unreal_open_mcp_actor_component_destroy",
      "unreal_open_mcp_actor_component_get",
      "unreal_open_mcp_actor_component_modify",
      "unreal_open_mcp_actor_component_list_all",
      "unreal_open_mcp_level_open",
      "unreal_open_mcp_level_save",
      "unreal_open_mcp_level_list_loaded",
      "unreal_open_mcp_level_set_current",
      "unreal_open_mcp_level_unload_sublevel",
      "unreal_open_mcp_level_get_data",
      "unreal_open_mcp_level_create",
      "unreal_open_mcp_validate_edit",
      "unreal_open_mcp_checkpoint_create",
      "unreal_open_mcp_delta",
      "unreal_open_mcp_apply_fix",
      "unreal_open_mcp_capabilities",
      "unreal_open_mcp_asset_find",
      "unreal_open_mcp_asset_get_data",
      "unreal_open_mcp_asset_create_folder",
      "unreal_open_mcp_asset_copy",
      "unreal_open_mcp_asset_move",
      "unreal_open_mcp_asset_delete",
      "unreal_open_mcp_asset_refresh",
      "unreal_open_mcp_material_create",
      "unreal_open_mcp_material_modify",
      "unreal_open_mcp_material_get_data",
      "unreal_open_mcp_asset_import",
      "unreal_open_mcp_editor_application_get_state",
      "unreal_open_mcp_editor_application_set_state",
      "unreal_open_mcp_editor_selection_get",
      "unreal_open_mcp_editor_selection_set",
      "unreal_open_mcp_console_get_logs",
      "unreal_open_mcp_console_clear_logs",
      "unreal_open_mcp_console_run_command",
      "unreal_open_mcp_reflection_method_find",
      "unreal_open_mcp_reflection_method_call",
      "unreal_open_mcp_screenshot_viewport",
      "unreal_open_mcp_screenshot_game_view",
      "unreal_open_mcp_screenshot_camera",
      "unreal_open_mcp_screenshot_isolated",
      "unreal_open_mcp_bridge_status",
      "unreal_open_mcp_blueprint_create",
      "unreal_open_mcp_blueprint_get",
      "unreal_open_mcp_blueprint_add_component",
      "unreal_open_mcp_blueprint_remove_component",
      "unreal_open_mcp_blueprint_add_variable",
      "unreal_open_mcp_blueprint_modify_variable",
      "unreal_open_mcp_blueprint_set_default",
      "unreal_open_mcp_blueprint_add_function",
      "unreal_open_mcp_blueprint_add_event",
      "unreal_open_mcp_blueprint_compile",
      "unreal_open_mcp_blueprint_spawn",
      "unreal_open_mcp_source_read",
      "unreal_open_mcp_source_list",
      "unreal_open_mcp_source_create_class",
      "unreal_open_mcp_source_update",
      "unreal_open_mcp_source_delete",
      "unreal_open_mcp_source_compile",
    ]);
  } finally {
    await cleanup();
  }
});

// --- healthy: full MCP round-trip returns the bridge PingResponse -----------

test("integration: tools/call ping returns the bridge health body on 200", async () => {
  const bridge = await startBridgeStub(HEALTHY_PING);
  try {
    const { client, cleanup } = await setupClient(bridge.port);
    try {
      const result = await client.callTool({
        name: "unreal_open_mcp_ping",
        arguments: {},
      });
      assert.equal(result.isError, false);
      // The PingResponse body survives the MCP round-trip verbatim — this is
      // the parity pin on the field set (unrealVersion, status, port, ...).
      assert.deepEqual(payloadOf(result), HEALTHY_PING);
    } finally {
      await cleanup();
    }
  } finally {
    await bridge.close();
  }
});

// --- bridge-down: no listener classifies as bridge_offline ------------------

test("integration: bridge-down tools/call surfaces bridge_offline with the lock hint", async () => {
  // Port 1 — nothing listening, ECONNREFUSED on connect. Must classify as
  // bridge_offline (NOT bridge_timeout) and name the instance lock dir so an
  // agent debugging the failure knows where to look. Pass a projectPath so the
  // offline hint names a concrete lock file (assertion below checks for it).
  const { client, cleanup } = await setupClient(1);
  setLiveRouter(new LiveClient(1, undefined, "/tmp/MyGame"));
  try {
    const result = await client.callTool({
      name: "unreal_open_mcp_ping",
      arguments: {},
    });
    assert.equal(result.isError, true);
    const body = bodyOf(result) as { error: { code: string; message: string } };
    assert.equal(body.error.code, "bridge_offline");
    assert.match(
      body.error.message,
      /\.unreal-open-mcp\/instances\//,
      "offline hint must name the instance lock dir",
    );
  } finally {
    await cleanup();
  }
});

// --- HTTP 500: reachable but errored surfaces as bridge_http_error ----------

test("integration: HTTP 500 tools/call surfaces bridge_http_error with the bridge body", async () => {
  const bridge = await startErrorBridgeStub(500, {
    error: { code: "internal", message: "boom" },
  });
  try {
    const { client, cleanup } = await setupClient(bridge.port);
    try {
      const result = await client.callTool({
        name: "unreal_open_mcp_ping",
        arguments: {},
      });
      assert.equal(result.isError, true);
      const body = bodyOf(result) as {
        error: { code: string; message: string };
      };
      // The bridge's own error body is surfaced verbatim so an agent sees the
      // real cause rather than an opaque HTTP status.
      assert.equal(body.error.code, "internal");
      assert.equal(body.error.message, "boom");
    } finally {
      await cleanup();
    }
  } finally {
    await bridge.close();
  }
});

// --- the server name is the published identity (parity pin) -----------------

test("integration: initialize reports the published server name", async () => {
  const { client, cleanup } = await setupClient(1);
  try {
    // The MCP Client captures the server's initialize response on connect; the
    // serverVersion is exposed as a public getter.
    const serverInfo = (client as unknown as {
      getServerVersion?: () => { name?: string } | undefined;
    }).getServerVersion?.();
    assert.equal(serverInfo?.name, SERVER_NAME);
  } finally {
    await cleanup();
  }
});

// ===========================================================================
// P2.8 — Phase 2 parity smoke (actor-find round-trip)
// ===========================================================================
//
// P1's integration tests pinned the /ping route only. Phase 2 widened the
// dispatch to every other tool via POST /tools/{name} with the canonical
// {ok, result, error} envelope (P2.1) and shipped the first typed tool,
// unreal_open_mcp_actor_find (P2.2). These cases pin the FULL typed-tool
// round-trip the way P1 pinned ping: MCP tools/call → LiveClient.postTool →
// POST /tools/unreal_open_mcp_actor_find → {ok,true,result} envelope → unwrapped
// result body surviving the MCP round-trip verbatim. Two failure modes are
// pinned alongside the healthy case: bridge-down surfaces the same
// `bridge_offline` envelope (proving the typed-tool path inherits P1's failure
// classification), and a {ok,false,error} envelope surfaces as a structured MCP
// error carrying the tool-specific code so an agent can branch on the cause.
//
// The stub here dispatches by method + URL — GET /ping stays healthy so a stray
// tools/call(ping) wouldn't crash, and POST /tools/unreal_open_mcp_actor_find
// returns the canonical actor-find result the bridge emits
// (FUnrealOpenMcpActorTools::HandleActorFind).

/**
 * Canonical actor-find targeted-hit body the bridge emits — pinned field set
 * for a single resolved actor (ToActorData with bIncludeComponents=true). The
 * stub returns this wrapped in the {ok:true,result:<body>} envelope; the MCP
 * `bodyOf` helper sees the INNER object after LiveClient.postTool unwraps it.
 */
const ACTOR_FIND_HIT = {
  actors: [
    {
      label: "PlayerStart",
      name: "PlayerStart",
      class: "/Script/Engine.PlayerStart",
      path: "/Game/Maps/Entry.Entry:PersistentLevel.PlayerStart",
      transform: {
        location: { x: 0, y: 0, z: 0 },
        rotation: { pitch: 0, yaw: 0, roll: 0 },
        scale: { x: 1, y: 1, z: 1 },
      },
      components: [{ name: "Sprite", class: "/Script/Engine.BillboardComponent" }],
    },
  ],
  notFound: false,
  count: 1,
};

/**
 * Bridge handler that serves GET /ping (healthy) AND POST
 * /tools/unreal_open_mcp_actor_find with the canonical envelope. Every other
 * request falls through to a 404 so a misrouted call surfaces as a clear
 * bridge_http_error rather than a false positive.
 */
async function actorFindHandler(
  req: IncomingMessage,
  res: ServerResponse,
): Promise<void> {
  if (req.method === "GET" && req.url === "/ping") {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(HEALTHY_PING));
    return;
  }
  if (req.method === "POST" && req.url === "/tools/unreal_open_mcp_actor_find") {
    // Drain the args body even though the stub ignores it — keeps the HTTP
    // exchange clean (the LiveClient writes a JSON body on every POST).
    await readBody(req);
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ ok: true, result: ACTOR_FIND_HIT }));
    return;
  }
  res.writeHead(404, { "Content-Type": "application/json" });
  res.end(
    JSON.stringify({
      error: { code: "not_found", message: `${req.method} ${req.url}` },
    }),
  );
}

// --- P2.8 healthy: full typed-tool round-trip unwraps the result body -------

test("P2.8 integration: tools/call actor_find returns the unwrapped result body on 200", async () => {
  const bridge = await startHandlerStub(actorFindHandler);
  try {
    const { client, cleanup } = await setupClient(bridge.port);
    try {
      const result = await client.callTool({
        name: "unreal_open_mcp_actor_find",
        arguments: { actor: "PlayerStart" },
      });
      // Success envelope → isError:false and the INNER result object (not the
      // {ok,result} wrapper) survives the MCP round-trip verbatim. This is the
      // parity pin on the actor-find field set the bridge contract pins.
      assert.equal(result.isError, false);
      assert.deepEqual(payloadOf(result), ACTOR_FIND_HIT);
    } finally {
      await cleanup();
    }
  } finally {
    await bridge.close();
  }
});

// --- P2.8 bridge-down: typed-tool path inherits bridge_offline --------------

test("P2.8 integration: bridge-down tools/call actor_find surfaces bridge_offline with the lock hint", async () => {
  // Port 1 — nothing listening. The typed-tool path MUST classify exactly like
  // the ping path: bridge_offline (NOT bridge_timeout), with the instance lock
  // path named so an agent debugging a port mismatch knows where to look. This
  // is the load-bearing assertion that the P1 failure classification survives
  // the P2.1 postTool route unchanged. Pass a projectPath via the router so the
  // offline hint names a concrete lock file (assertion below checks for it).
  const { client, cleanup } = await setupClient(1);
  setLiveRouter(new LiveClient(1, undefined, "/tmp/MyGame"));
  try {
    const result = await client.callTool({
      name: "unreal_open_mcp_actor_find",
      arguments: { actor: "PlayerStart" },
    });
    assert.equal(result.isError, true);
    const body = bodyOf(result) as { error: { code: string; message: string } };
    assert.equal(body.error.code, "bridge_offline");
    assert.match(
      body.error.message,
      /\.unreal-open-mcp\/instances\//,
      "offline hint must name the instance lock dir",
    );
  } finally {
    await cleanup();
  }
});

// --- P2.8 tool error: {ok,false,error} surfaces as a structured MCP error ----

test("P2.8 integration: tools/call actor_find surfaces the tool error envelope on ok:false", async () => {
  // The bridge ran the handler and it returned a structured failure (e.g. the
  // referenced actor does not resolve, or no editor world is loaded). The
  // {ok:false,error:{code,message}} envelope must surface as an MCP error
  // (isError:true) carrying the tool-specific code verbatim so an agent can
  // branch on actor_not_found vs invalid_parameter vs no_editor_world.
  const bridge = await startHandlerStub(async (req, res) => {
    if (req.method === "POST" && req.url === "/tools/unreal_open_mcp_actor_find") {
      await readBody(req);
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(
        JSON.stringify({
          ok: false,
          error: {
            code: "actor_not_found",
            message: "No actor resolved for ref 'DoesNotExist'.",
          },
        }),
      );
      return;
    }
    res.writeHead(404, { "Content-Type": "application/json" });
    res.end(
      JSON.stringify({
        error: { code: "not_found", message: `${req.method} ${req.url}` },
      }),
    );
  });
  try {
    const { client, cleanup } = await setupClient(bridge.port);
    try {
      const result = await client.callTool({
        name: "unreal_open_mcp_actor_find",
        arguments: { actor: "DoesNotExist" },
      });
      assert.equal(result.isError, true);
      const body = bodyOf(result) as {
        error: { code: string; message: string };
      };
      // The tool-specific error code rides through — an agent can branch on
      // actor_not_found rather than seeing an opaque transport error.
      assert.equal(body.error.code, "actor_not_found");
      assert.equal(
        body.error.message,
        "No actor resolved for ref 'DoesNotExist'.",
      );
    } finally {
      await cleanup();
    }
  } finally {
    await bridge.close();
  }
});

// ===========================================================================
// P4.5 — Phase 4 parity smoke (asset-find round-trip)
// ===========================================================================
//
// P2.8 pinned the first typed-tool round-trip (actor_find) through POST
// /tools/{name} with the {ok,result,error} envelope. Phase 4 shipped the
// asset family; P4.5 is the mandatory phase-gate before Phase 5 and proves
// one asset-family tool survives the full MCP ↔ bridge path. asset_find is the
// smoke default (read-only, no gate — the safest tool to prove the wiring
// without a checkpoint/mutate dance).
//
// These cases mirror the P2.8 structure exactly, only swapping the tool name
// and the canonical result body: healthy (the paginated {total,offset,count,
// assets} body survives the round-trip verbatim after LiveClient.postTool
// unwraps the envelope), bridge-down (the asset path inherits P1's
// bridge_offline classification with the instance-lock hint), and tool-error
// ({ok:false,error} surfaces as an MCP error carrying the tool-specific
// invalid_class_path code so an agent can branch on the cause).

/**
 * Canonical asset-find result body the bridge emits — one resolved material
 * asset under /Game. Pinned field set for a single AssetSummary
 * ({ name, path, package, class }) wrapped in the offset/limit pagination
 * envelope. The stub returns this inside {ok:true,result:<body>}; the MCP
 * `bodyOf` helper sees the INNER object after LiveClient.postTool unwraps it.
 */
const ASSET_FIND_HIT = {
  total: 1,
  offset: 0,
  count: 1,
  assets: [
    {
      name: "M_Test",
      path: "/Game/M_Test.M_Test",
      package: "/Game/M_Test",
      class: "/Script/Engine.Material",
    },
  ],
};

/**
 * Bridge handler that serves GET /ping (healthy) AND POST
 * /tools/unreal_open_mcp_asset_find with the canonical envelope. Every other
 * request falls through to a 404 so a misrouted call surfaces as a clear
 * bridge_http_error rather than a false positive.
 */
async function assetFindHandler(
  req: IncomingMessage,
  res: ServerResponse,
): Promise<void> {
  if (req.method === "GET" && req.url === "/ping") {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(HEALTHY_PING));
    return;
  }
  if (req.method === "POST" && req.url === "/tools/unreal_open_mcp_asset_find") {
    await readBody(req);
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ ok: true, result: ASSET_FIND_HIT }));
    return;
  }
  res.writeHead(404, { "Content-Type": "application/json" });
  res.end(
    JSON.stringify({
      error: { code: "not_found", message: `${req.method} ${req.url}` },
    }),
  );
}

// --- P4.5 healthy: full typed-tool round-trip unwraps the result body -------

test("P4.5 integration: tools/call asset_find returns the unwrapped result body on 200", async () => {
  const bridge = await startHandlerStub(assetFindHandler);
  try {
    const { client, cleanup } = await setupClient(bridge.port);
    try {
      const result = await client.callTool({
        name: "unreal_open_mcp_asset_find",
        arguments: { path: "/Game", limit: 10 },
      });
      // Success envelope → isError:false and the INNER result object (not the
      // {ok,result} wrapper) survives the MCP round-trip verbatim — the parity
      // pin on the asset-find pagination + AssetSummary field set.
      assert.equal(result.isError, false);
      assert.deepEqual(payloadOf(result), ASSET_FIND_HIT);
    } finally {
      await cleanup();
    }
  } finally {
    await bridge.close();
  }
});

// --- P4.5 bridge-down: asset path inherits bridge_offline -------------------

test("P4.5 integration: bridge-down tools/call asset_find surfaces bridge_offline with the lock hint", async () => {
  // Port 1 — nothing listening. The asset path MUST classify exactly like the
  // ping / actor_find paths: bridge_offline (NOT bridge_timeout), with the
  // instance lock path named so an agent debugging a port mismatch knows where
  // to look. Pass a projectPath via the router so the offline hint names a
  // concrete lock file (assertion below checks for it).
  const { client, cleanup } = await setupClient(1);
  setLiveRouter(new LiveClient(1, undefined, "/tmp/MyGame"));
  try {
    const result = await client.callTool({
      name: "unreal_open_mcp_asset_find",
      arguments: { path: "/Game", limit: 10 },
    });
    assert.equal(result.isError, true);
    const body = bodyOf(result) as { error: { code: string; message: string } };
    assert.equal(body.error.code, "bridge_offline");
    assert.match(
      body.error.message,
      /\.unreal-open-mcp\/instances\//,
      "offline hint must name the instance lock dir",
    );
  } finally {
    await cleanup();
  }
});

// --- P4.5 tool error: {ok,false,error} surfaces as a structured MCP error ----

test("P4.5 integration: tools/call asset_find surfaces the tool error envelope on ok:false", async () => {
  // The bridge ran the handler and it returned a structured failure — here a
  // malformed class_path (short dotless name rejected before the registry
  // query). The {ok:false,error:{code,message}} envelope must surface as an
  // MCP error (isError:true) carrying the tool-specific code verbatim so an
  // agent can branch on invalid_class_path vs invalid_parameter.
  const bridge = await startHandlerStub(async (req, res) => {
    if (req.method === "POST" && req.url === "/tools/unreal_open_mcp_asset_find") {
      await readBody(req);
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(
        JSON.stringify({
          ok: false,
          error: {
            code: "invalid_class_path",
            message:
              "class_path 'Material' is not a '/Script/Module.Class' path.",
          },
        }),
      );
      return;
    }
    res.writeHead(404, { "Content-Type": "application/json" });
    res.end(
      JSON.stringify({
        error: { code: "not_found", message: `${req.method} ${req.url}` },
      }),
    );
  });
  try {
    const { client, cleanup } = await setupClient(bridge.port);
    try {
      const result = await client.callTool({
        name: "unreal_open_mcp_asset_find",
        arguments: { class_path: "Material" },
      });
      assert.equal(result.isError, true);
      const body = bodyOf(result) as {
        error: { code: string; message: string };
      };
      // The tool-specific error code rides through — an agent can branch on
      // invalid_class_path rather than seeing an opaque transport error.
      assert.equal(body.error.code, "invalid_class_path");
      assert.equal(
        body.error.message,
        "class_path 'Material' is not a '/Script/Module.Class' path.",
      );
    } finally {
      await cleanup();
    }
  } finally {
    await bridge.close();
  }
});

// ===========================================================================
// P6.6 — Phase 6 parity smoke (blueprint compile-loop: spawn + compile)
// ===========================================================================
//
// Phase 6 shipped the Blueprint family; P6.6 is the mandatory phase-gate
// before Phase 7 and closes the create -> edit -> compile -> spawn loop. The
// P6.6 cases pin the two compile-loop tools (blueprint_compile +
// blueprint_spawn) through the full MCP ↔ bridge path and add the CRITICAL
// compile-failure data-path assertion the P6.5 contract rides on: a
// succeeded:false compile is a NORMAL result (ok:true envelope, MCP
// isError:false), NOT a transport error.
//
// These cases mirror the P2.8 / P4.5 structure exactly, swapping in the
// Blueprint-family tools. spawn is the loop-closer (mutating, gate Enforce);
// compile is the AI feedback loop (mutating, gate Enforce, succeeded:false is
// data). The compile-failure case is P6.5's load-bearing contract asserted at
// the MCP layer for the first time.

/**
 * Canonical blueprint_spawn result body the bridge emits — minimal actor
 * identity for the agent to chain from. The stub returns this inside
 * {ok:true,result:<body>}; the MCP `bodyOf` helper sees the INNER object
 * after LiveClient.postTool unwraps the envelope.
 */
const BLUEPRINT_SPAWN_OK = {
  actor: "BP_Smoke",
  name: "BP_Smoke_C_0",
  class: "/Game/McpTemp/BP_Smoke.BP_Smoke_C",
  path: "/Game/Maps/UEDPIE_0_TestMap.TestMap:PersistentLevel.BP_Smoke_C_0",
  location: { x: 0, y: 0, z: 100 },
};

/**
 * Canonical blueprint_compile CLEAN result body — succeeded:true + numErrors:0
 * + empty messages[] on an ok:true envelope.
 */
const BLUEPRINT_COMPILE_CLEAN = {
  succeeded: true,
  numErrors: 0,
  numWarnings: 0,
  messages: [],
};

/**
 * Canonical blueprint_compile FAILED result body — succeeded:false + a
 * populated messages[] on an ok:true envelope. This is the P6.5 contract: a
 * failed compile is a NORMAL result, NOT a transport failure, so MCP
 * isError MUST stay false and the diagnostics ride through as data.
 */
const BLUEPRINT_COMPILE_FAILED = {
  succeeded: false,
  numErrors: 1,
  numWarnings: 0,
  messages: [
    {
      severity: "error",
      message: "Foo node: pin 'A' is not connected.",
      node: "Foo",
      graph: "EventGraph",
    },
  ],
};

/**
 * Bridge handler that serves GET /ping (healthy) AND POST
 * /tools/unreal_open_mcp_blueprint_spawn with the canonical ok:true envelope.
 * Used by the P6.6 spawn round-trip.
 */
async function blueprintSpawnHandler(
  req: IncomingMessage,
  res: ServerResponse,
): Promise<void> {
  if (req.method === "GET" && req.url === "/ping") {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(HEALTHY_PING));
    return;
  }
  if (
    req.method === "POST" &&
    req.url === "/tools/unreal_open_mcp_blueprint_spawn"
  ) {
    await readBody(req);
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ ok: true, result: BLUEPRINT_SPAWN_OK }));
    return;
  }
  res.writeHead(404, { "Content-Type": "application/json" });
  res.end(
    JSON.stringify({
      error: { code: "not_found", message: `${req.method} ${req.url}` },
    }),
  );
}

// --- P6.6 healthy: blueprint_spawn round-trip unwraps the result body -------

test("P6.6 integration: tools/call blueprint_spawn returns the unwrapped result body on 200", async () => {
  const bridge = await startHandlerStub(blueprintSpawnHandler);
  try {
    const { client, cleanup } = await setupClient(bridge.port);
    try {
      const result = await client.callTool({
        name: "unreal_open_mcp_blueprint_spawn",
        arguments: {
          path: "/Game/McpTemp/BP_Smoke",
          location: { x: 0, y: 0, z: 100 },
          paths_hint: ["/Game/McpTemp/BP_Smoke"],
        },
      });
      // Success envelope → isError:false and the INNER result object (not the
      // {ok,result} wrapper) survives the MCP round-trip verbatim — the parity
      // pin on the spawn identity field set ({ actor, name, class, path,
      // location }).
      assert.equal(result.isError, false);
      assert.deepEqual(payloadOf(result), BLUEPRINT_SPAWN_OK);
    } finally {
      await cleanup();
    }
  } finally {
    await bridge.close();
  }
});

// --- P6.6 compile-failure data path: succeeded:false stays isError:false ----
//
// The P6.5 contract: a FAILED compile is a NORMAL, expected result, NOT a
// transport failure. The bridge keeps the envelope ok:true and carries
// succeeded:false + the populated messages[] so an agent reads the
// diagnostics and recompiles. At the MCP layer that means isError MUST stay
// false — the diagnostics are data, not an error. This case pins that
// invariant at the MCP boundary for the first time (P6.5 pinned it at the
// bridge handler level only).

test("P6.6 integration: tools/call blueprint_compile with succeeded:false stays isError:false (compile failure is data, not a transport error)", async () => {
  const bridge = await startHandlerStub(async (req, res) => {
    if (req.method === "GET" && req.url === "/ping") {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify(HEALTHY_PING));
      return;
    }
    if (
      req.method === "POST" &&
      req.url === "/tools/unreal_open_mcp_blueprint_compile"
    ) {
      await readBody(req);
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ ok: true, result: BLUEPRINT_COMPILE_FAILED }));
      return;
    }
    res.writeHead(404, { "Content-Type": "application/json" });
    res.end(
      JSON.stringify({
        error: { code: "not_found", message: `${req.method} ${req.url}` },
      }),
    );
  });
  try {
    const { client, cleanup } = await setupClient(bridge.port);
    try {
      const result = await client.callTool({
        name: "unreal_open_mcp_blueprint_compile",
        arguments: {
          path: "/Game/McpTemp/BP_Broken",
          paths_hint: ["/Game/McpTemp/BP_Broken"],
        },
      });
      // CRITICAL P6.5 invariant at the MCP boundary: a failed compile is
      // data, not an error. isError stays false and the diagnostics ride
      // through on the ok:true result object.
      assert.equal(
        result.isError,
        false,
        "succeeded:false compile must surface as isError:false (data, not transport error)",
      );
      const body = bodyOf(result) as typeof BLUEPRINT_COMPILE_FAILED;
      assert.equal(body.succeeded, false);
      assert.equal(body.numErrors, 1);
      assert.ok(Array.isArray(body.messages) && body.messages.length === 1);
      assert.equal(body.messages[0].severity, "error");
    } finally {
      await cleanup();
    }
  } finally {
    await bridge.close();
  }
});

// --- P6.6 clean compile round-trip -------------------------------------------

test("P6.6 integration: tools/call blueprint_compile returns the clean result body on 200", async () => {
  const bridge = await startHandlerStub(async (req, res) => {
    if (req.method === "GET" && req.url === "/ping") {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify(HEALTHY_PING));
      return;
    }
    if (
      req.method === "POST" &&
      req.url === "/tools/unreal_open_mcp_blueprint_compile"
    ) {
      await readBody(req);
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ ok: true, result: BLUEPRINT_COMPILE_CLEAN }));
      return;
    }
    res.writeHead(404, { "Content-Type": "application/json" });
    res.end(
      JSON.stringify({
        error: { code: "not_found", message: `${req.method} ${req.url}` },
      }),
    );
  });
  try {
    const { client, cleanup } = await setupClient(bridge.port);
    try {
      const result = await client.callTool({
        name: "unreal_open_mcp_blueprint_compile",
        arguments: {
          path: "/Game/McpTemp/BP_Clean",
          paths_hint: ["/Game/McpTemp/BP_Clean"],
        },
      });
      assert.equal(result.isError, false);
      assert.deepEqual(payloadOf(result), BLUEPRINT_COMPILE_CLEAN);
    } finally {
      await cleanup();
    }
  } finally {
    await bridge.close();
  }
});

// --- P6.6 bridge-down: blueprint_spawn inherits bridge_offline --------------

test("P6.6 integration: bridge-down tools/call blueprint_spawn surfaces bridge_offline with the lock hint", async () => {
  // Port 1 — nothing listening. The blueprint path inherits P1's
  // bridge_offline classification with the instance-lock hint, exactly like
  // the actor_find / asset_find paths.
  const { client, cleanup } = await setupClient(1);
  setLiveRouter(new LiveClient(1, undefined, "/tmp/MyGame"));
  try {
    const result = await client.callTool({
      name: "unreal_open_mcp_blueprint_spawn",
      arguments: {
        path: "/Game/McpTemp/BP_Smoke",
        paths_hint: ["/Game/McpTemp/BP_Smoke"],
      },
    });
    assert.equal(result.isError, true);
    const body = bodyOf(result) as { error: { code: string; message: string } };
    assert.equal(body.error.code, "bridge_offline");
    assert.match(
      body.error.message,
      /\.unreal-open-mcp\/instances\//,
      "offline hint must name the instance lock dir",
    );
  } finally {
    await cleanup();
  }
});

// --- P6.6 tool error: {ok,false,error} surfaces as a structured MCP error ----

test("P6.6 integration: tools/call blueprint_spawn surfaces the tool error envelope on ok:false (not_compiled)", async () => {
  // The bridge ran the handler and it returned a structured failure — here an
  // uncompiled Blueprint (no GeneratedClass). The {ok:false,error:{code,
  // message}} envelope must surface as an MCP error (isError:true) carrying
  // the tool-specific not_compiled code so an agent can branch on the cause
  // and run blueprint_compile instead of retrying blindly.
  const bridge = await startHandlerStub(async (req, res) => {
    if (
      req.method === "POST" &&
      req.url === "/tools/unreal_open_mcp_blueprint_spawn"
    ) {
      await readBody(req);
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(
        JSON.stringify({
          ok: false,
          error: {
            code: "not_compiled",
            message:
              "Blueprint 'BP_New' has no GeneratedClass — run blueprint_compile first.",
          },
        }),
      );
      return;
    }
    res.writeHead(404, { "Content-Type": "application/json" });
    res.end(
      JSON.stringify({
        error: { code: "not_found", message: `${req.method} ${req.url}` },
      }),
    );
  });
  try {
    const { client, cleanup } = await setupClient(bridge.port);
    try {
      const result = await client.callTool({
        name: "unreal_open_mcp_blueprint_spawn",
        arguments: {
          path: "/Game/McpTemp/BP_New",
          paths_hint: ["/Game/McpTemp/BP_New"],
        },
      });
      assert.equal(result.isError, true);
      const body = bodyOf(result) as {
        error: { code: string; message: string };
      };
      // The tool-specific error code rides through — an agent can branch on
      // not_compiled (run blueprint_compile) rather than seeing an opaque
      // transport error.
      assert.equal(body.error.code, "not_compiled");
      assert.equal(
        body.error.message,
        "Blueprint 'BP_New' has no GeneratedClass — run blueprint_compile first.",
      );
    } finally {
      await cleanup();
    }
  } finally {
    await bridge.close();
  }
});

// ===========================================================================
// P7.4 — Phase 7 parity smoke (source compile-loop: update + compile)
// ===========================================================================
//
// Phase 7 shipped the Source family; P7.4 is the mandatory phase-gate before
// Phase 8 and closes the C++ edit -> compile loop. The P7.4 cases pin the two
// load-bearing source tools (source_update + source_compile) through the full
// MCP <-> bridge path and add the CRITICAL compile-failure data-path
// assertion the P7.3 contract rides on: a success:false compile is a NORMAL
// result (ok:true envelope, MCP isError:false), NOT a transport error — the
// same contract P6.6 pinned for Blueprint compiles, now asserted at the MCP
// boundary for the C++ loop.
//
// These cases mirror the P6.6 structure exactly, swapping in the Source-
// family tools. source_update is the C++ edit step (mutating, gate Enforce,
// jailed to <Project>/Source/); source_compile is the AI feedback loop
// (mutating, gate Enforce, returns a structured UBT/Live-Coding diagnostic
// report with success SPLIT from compile_clean — a loaded editor holds its
// module DLL so success:false + compile_clean:true is expected). The
// compile-failure case is P7.3's load-bearing contract asserted at the MCP
// layer for the first time; the tool-error case pins the P7.1 Source/ jail
// rejection (path_escapes_jail).

/**
 * Canonical source_update result body the bridge emits — the write identity
 * the agent chains from ({path, mode, bytes_written, lines_written}). The
 * stub returns this inside {ok:true,result:<body>}; the MCP `bodyOf` helper
 * sees the INNER object after LiveClient.postTool unwraps the envelope.
 */
const SOURCE_UPDATE_OK = {
  path: "MyGame/McpSmoke.cpp",
  mode: "full",
  bytes_written: 42,
  lines_written: 3,
};

/**
 * Canonical source_compile CLEAN result body — a UBT report with success:true
 * + compile_clean:true + error_count:0 + empty diagnostics on an ok:true
 * envelope. The method:'ubt' tag distinguishes the deterministic build path
 * from Live Coding (method:'live_coding').
 */
const SOURCE_COMPILE_CLEAN = {
  method: "ubt",
  target: "MyGameEditor",
  configuration: "Development",
  platform: "Win64",
  return_code: 0,
  success: true,
  compile_clean: true,
  error_count: 0,
  warning_count: 0,
  diagnostics: [],
};

/**
 * Canonical source_compile FAILED result body — success:false +
 * compile_clean:false + a populated diagnostics[] on an ok:true envelope.
 * This is the P7.3 contract: a failed compile is a NORMAL result, NOT a
 * transport failure, so MCP isError MUST stay false and the diagnostics ride
 * through as data (the same contract P6.6 pinned for Blueprint compiles).
 */
const SOURCE_COMPILE_FAILED = {
  method: "ubt",
  target: "MyGameEditor",
  configuration: "Development",
  platform: "Win64",
  return_code: 1,
  success: false,
  compile_clean: false,
  error_count: 1,
  warning_count: 0,
  diagnostics: [
    {
      file: "MyGame/McpSmoke.cpp",
      line: 7,
      severity: "error",
      message: "expected a ';'",
    },
  ],
};

/**
 * Bridge handler that serves GET /ping (healthy) AND POST
 * /tools/unreal_open_mcp_source_update with the canonical ok:true envelope.
 * Used by the P7.4 update round-trip.
 */
async function sourceUpdateHandler(
  req: IncomingMessage,
  res: ServerResponse,
): Promise<void> {
  if (req.method === "GET" && req.url === "/ping") {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(HEALTHY_PING));
    return;
  }
  if (
    req.method === "POST" &&
    req.url === "/tools/unreal_open_mcp_source_update"
  ) {
    await readBody(req);
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ ok: true, result: SOURCE_UPDATE_OK }));
    return;
  }
  res.writeHead(404, { "Content-Type": "application/json" });
  res.end(
    JSON.stringify({
      error: { code: "not_found", message: `${req.method} ${req.url}` },
    }),
  );
}

// --- P7.4 healthy: source_update round-trip unwraps the result body --------

test("P7.4 integration: tools/call source_update returns the unwrapped result body on 200", async () => {
  const bridge = await startHandlerStub(sourceUpdateHandler);
  try {
    const { client, cleanup } = await setupClient(bridge.port);
    try {
      const result = await client.callTool({
        name: "unreal_open_mcp_source_update",
        arguments: {
          path: "MyGame/McpSmoke.cpp",
          content: "// mcp smoke\nint32 main() { return 0; }\n",
          paths_hint: ["MyGame/McpSmoke.cpp"],
        },
      });
      // Success envelope -> isError:false and the INNER result object (not the
      // {ok,result} wrapper) survives the MCP round-trip verbatim — the parity
      // pin on the update identity field set ({path, mode, bytes_written,
      // lines_written}).
      assert.equal(result.isError, false);
      assert.deepEqual(payloadOf(result), SOURCE_UPDATE_OK);
    } finally {
      await cleanup();
    }
  } finally {
    await bridge.close();
  }
});

// --- P7.4 healthy: source_compile clean round-trip -------------------------

test("P7.4 integration: tools/call source_compile returns the clean UBT report body on 200", async () => {
  const bridge = await startHandlerStub(async (req, res) => {
    if (req.method === "GET" && req.url === "/ping") {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify(HEALTHY_PING));
      return;
    }
    if (
      req.method === "POST" &&
      req.url === "/tools/unreal_open_mcp_source_compile"
    ) {
      await readBody(req);
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ ok: true, result: SOURCE_COMPILE_CLEAN }));
      return;
    }
    res.writeHead(404, { "Content-Type": "application/json" });
    res.end(
      JSON.stringify({
        error: { code: "not_found", message: `${req.method} ${req.url}` },
      }),
    );
  });
  try {
    const { client, cleanup } = await setupClient(bridge.port);
    try {
      const result = await client.callTool({
        name: "unreal_open_mcp_source_compile",
        arguments: {
          use_live_coding: false,
          paths_hint: ["MyGame/McpSmoke.cpp"],
        },
      });
      assert.equal(result.isError, false);
      assert.deepEqual(payloadOf(result), SOURCE_COMPILE_CLEAN);
    } finally {
      await cleanup();
    }
  } finally {
    await bridge.close();
  }
});

// --- P7.4 compile-failure data path: success:false stays isError:false -----
//
// The P7.3 contract: a FAILED C++ compile is a NORMAL, expected result, NOT a
// transport failure. The bridge keeps the envelope ok:true and carries
// success:false + compile_clean:false + the populated diagnostics[] so an
// agent reads the diagnostics and recompiles. At the MCP layer that means
// isError MUST stay false — the diagnostics are data, not an error. This case
// pins that invariant at the MCP boundary for the first time (P7.3 pinned it
// at the bridge handler / ParseDiagnostics level only).

test("P7.4 integration: tools/call source_compile with success:false stays isError:false (compile failure is data, not a transport error)", async () => {
  const bridge = await startHandlerStub(async (req, res) => {
    if (req.method === "GET" && req.url === "/ping") {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify(HEALTHY_PING));
      return;
    }
    if (
      req.method === "POST" &&
      req.url === "/tools/unreal_open_mcp_source_compile"
    ) {
      await readBody(req);
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ ok: true, result: SOURCE_COMPILE_FAILED }));
      return;
    }
    res.writeHead(404, { "Content-Type": "application/json" });
    res.end(
      JSON.stringify({
        error: { code: "not_found", message: `${req.method} ${req.url}` },
      }),
    );
  });
  try {
    const { client, cleanup } = await setupClient(bridge.port);
    try {
      const result = await client.callTool({
        name: "unreal_open_mcp_source_compile",
        arguments: {
          use_live_coding: false,
          paths_hint: ["MyGame/McpSmoke.cpp"],
        },
      });
      // CRITICAL P7.3 invariant at the MCP boundary: a failed compile is
      // data, not an error. isError stays false and the diagnostics ride
      // through on the ok:true result object.
      assert.equal(
        result.isError,
        false,
        "success:false compile must surface as isError:false (data, not transport error)",
      );
      const body = bodyOf(result) as typeof SOURCE_COMPILE_FAILED;
      assert.equal(body.success, false);
      assert.equal(body.compile_clean, false);
      assert.equal(body.error_count, 1);
      assert.ok(
        Array.isArray(body.diagnostics) && body.diagnostics.length === 1,
      );
      assert.equal(body.diagnostics[0].severity, "error");
      assert.equal(body.diagnostics[0].file, "MyGame/McpSmoke.cpp");
      assert.equal(typeof body.diagnostics[0].line, "number");
      assert.equal(typeof body.diagnostics[0].message, "string");
    } finally {
      await cleanup();
    }
  } finally {
    await bridge.close();
  }
});

// --- P7.4 bridge-down: source_update inherits bridge_offline ----------------

test("P7.4 integration: bridge-down tools/call source_update surfaces bridge_offline with the lock hint", async () => {
  // Port 1 — nothing listening. The source_update path inherits P1's
  // bridge_offline classification with the instance-lock hint, exactly like
  // the actor_find / asset_find / blueprint_spawn paths.
  const { client, cleanup } = await setupClient(1);
  setLiveRouter(new LiveClient(1, undefined, "/tmp/MyGame"));
  try {
    const result = await client.callTool({
      name: "unreal_open_mcp_source_update",
      arguments: {
        path: "MyGame/McpSmoke.cpp",
        content: "// mcp smoke\n",
        paths_hint: ["MyGame/McpSmoke.cpp"],
      },
    });
    assert.equal(result.isError, true);
    const body = bodyOf(result) as { error: { code: string; message: string } };
    assert.equal(body.error.code, "bridge_offline");
    assert.match(
      body.error.message,
      /\.unreal-open-mcp\/instances\//,
      "offline hint must name the instance lock dir",
    );
  } finally {
    await cleanup();
  }
});

// --- P7.4 tool error: {ok,false,error} surfaces as a structured MCP error ----

test("P7.4 integration: tools/call source_update surfaces the tool error envelope on ok:false (path_escapes_jail)", async () => {
  // The bridge ran the handler and it returned a structured failure — here a
  // Source/ jail escape (an agent passed ../ or an absolute path outside the
  // project's Source/). The {ok:false,error:{code,message}} envelope must
  // surface as an MCP error (isError:true) carrying the tool-specific
  // path_escapes_jail code so an agent can branch on the cause (rewrite the
  // path inside Source/) instead of retrying blindly.
  const bridge = await startHandlerStub(async (req, res) => {
    if (
      req.method === "POST" &&
      req.url === "/tools/unreal_open_mcp_source_update"
    ) {
      await readBody(req);
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(
        JSON.stringify({
          ok: false,
          error: {
            code: "path_escapes_jail",
            message:
              "Path '../Engine/Source/Runtime/Core/Private/Core.cpp' escapes the project Source/ jail.",
          },
        }),
      );
      return;
    }
    res.writeHead(404, { "Content-Type": "application/json" });
    res.end(
      JSON.stringify({
        error: { code: "not_found", message: `${req.method} ${req.url}` },
      }),
    );
  });
  try {
    const { client, cleanup } = await setupClient(bridge.port);
    try {
      const result = await client.callTool({
        name: "unreal_open_mcp_source_update",
        arguments: {
          path: "../Engine/Source/Runtime/Core/Private/Core.cpp",
          content: "// should be rejected\n",
          paths_hint: ["../Engine/Source/Runtime/Core/Private/Core.cpp"],
        },
      });
      assert.equal(result.isError, true);
      const body = bodyOf(result) as {
        error: { code: string; message: string };
      };
      // The tool-specific error code rides through — an agent can branch on
      // path_escapes_jail (rewrite the path inside Source/) rather than seeing
      // an opaque transport error.
      assert.equal(body.error.code, "path_escapes_jail");
      assert.equal(
        body.error.message,
        "Path '../Engine/Source/Runtime/Core/Private/Core.cpp' escapes the project Source/ jail.",
      );
    } finally {
      await cleanup();
    }
  } finally {
    await bridge.close();
  }
});
