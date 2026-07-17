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
    // inspect + create pair — level_get_data / level_create). Guard against
    // accidental registry drift silently changing what the phase-gate smoke
    // covers.
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
      assert.deepEqual(bodyOf(result), HEALTHY_PING);
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
      assert.deepEqual(bodyOf(result), ACTOR_FIND_HIT);
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
