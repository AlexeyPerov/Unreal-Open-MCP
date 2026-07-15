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
    // P1 registry is exactly ping — guard against accidental registry bloat
    // silently changing what the phase-gate smoke covers.
    assert.deepEqual(names, ["unreal_open_mcp_ping"]);
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
