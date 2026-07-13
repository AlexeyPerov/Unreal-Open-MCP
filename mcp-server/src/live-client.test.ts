// LiveClient ping-path tests (P1.7).
//
// Drives a real local HTTP "bridge" stub through every /ping outcome the
// acceptance criteria name: healthy, 503 (not ready), timeout (abort),
// offline (ECONNREFUSED), HTTP 500, malformed body, and the non-ping route
// (tool_not_routed). The stub mirrors the Unreal bridge's pinned /ping JSON
// shape (FUnrealOpenMcpBridgeJson::BuildPingJson) so the success-path test
// doubles as a parity pin on the field set.
//
// Pattern adapted from Unity Open MCP's mcp-server/src/live-client.test.ts
// (copy fidelity, P1.7): an ephemeral node:http server + a small helper to
// swap the request handler on the fly.

import { test } from "node:test";
import assert from "node:assert/strict";
import {
  createServer,
  type Server as HttpServer,
  type IncomingMessage,
  type ServerResponse,
} from "node:http";
import { LiveClient, isAbortError } from "./live-client.js";

/** Canonical 200 /ping body the Unreal bridge emits (pinned field order). */
const HEALTHY_PING = {
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
  /** Replace the response handler on the fly. */
  setHandler(fn: (req: IncomingMessage, res: ServerResponse) => void): void;
}

function startBridgeStub(
  initialHandler: (req: IncomingMessage, res: ServerResponse) => void,
): Promise<BridgeStub> {
  return new Promise((resolve) => {
    let handler = initialHandler;
    const server = createServer((req, res) => handler(req, res));
    server.listen(0, "127.0.0.1", () => {
      const addr = server.address();
      const port = typeof addr === "object" && addr ? addr.port : 0;
      resolve({
        server,
        port,
        close: () => new Promise<void>((r) => server.close(() => r())),
        setHandler: (fn) => {
          handler = fn;
        },
      });
    });
  });
}

/** Handler that answers /ping with the healthy body. */
function healthyHandler(_req: IncomingMessage, res: ServerResponse): void {
  res.writeHead(200, { "Content-Type": "application/json" });
  res.end(JSON.stringify(HEALTHY_PING));
}

/** Handler that answers /ping with a 503 not-ready fallback body. */
function notReadyHandler(_req: IncomingMessage, res: ServerResponse): void {
  res.writeHead(503, { "Content-Type": "application/json" });
  res.end(
    JSON.stringify({
      ...HEALTHY_PING,
      connected: false,
      status: "not_ready",
      compiling: true,
    }),
  );
}

/** Parse the first text block of a CallToolResult as JSON. */
function bodyOf(result: { content: Array<{ type: string; text?: string }> }): unknown {
  const block = result.content[0];
  assert.ok(block?.type === "text", "first content block must be text");
  return JSON.parse(block.text as string);
}

// --- healthy path -----------------------------------------------------------

test("ping: returns the bridge health body on 200", async () => {
  const bridge = await startBridgeStub(healthyHandler);
  try {
    const client = new LiveClient(bridge.port);
    const result = await client.route("unreal_open_mcp_ping", {});
    assert.equal(result.isError, false);
    assert.deepEqual(bodyOf(result), HEALTHY_PING);
  } finally {
    await bridge.close();
  }
});

// --- 503 not-ready ---------------------------------------------------------

test("ping: 503 surfaces as bridge_http_error carrying the not-ready body", async () => {
  const bridge = await startBridgeStub(notReadyHandler);
  try {
    const client = new LiveClient(bridge.port);
    const result = await client.route("unreal_open_mcp_ping", {});
    assert.equal(result.isError, true);
    const body = bodyOf(result) as {
      connected: boolean;
      status: string;
      compiling: boolean;
    };
    // The bridge's fallback body is surfaced so an agent sees connected:false
    // + not_ready rather than an opaque HTTP status.
    assert.equal(body.connected, false);
    assert.equal(body.status, "not_ready");
    assert.equal(body.compiling, true);
  } finally {
    await bridge.close();
  }
});

// --- timeout (abort) -------------------------------------------------------

test("ping: a bridge that never responds classifies as bridge_timeout", async () => {
  // Handler that accepts the connection but never responds — the client's
  // AbortController timeout fires. A listener is required (ECONNREFUSED would
  // be bridge_offline, not timeout); the handler keeps the socket open until
  // the client aborts.
  const bridge = await startBridgeStub((_req, _res) => {
    // Never respond.
  });
  try {
    // Production uses PING_TIMEOUT_MS (5s). The timeout is exposed via a
    // protected getPingTimeoutMs() override so this test drives a real abort in
    // 150ms while exercising the exact same code path
    // (handlePing → fetchWithTimeout → abort → isAbortError).
    const fastClient = new (class extends LiveClient {
      protected getPingTimeoutMs(): number {
        return 150;
      }
    })(bridge.port);
    const result = await fastClient.route("unreal_open_mcp_ping", {});
    assert.equal(result.isError, true);
    const body = bodyOf(result) as { error: { code: string } };
    assert.equal(body.error.code, "bridge_timeout");
  } finally {
    await bridge.close();
  }
});

test("isAbortError: classifies DOMException AbortError and rejects others", () => {
  const abort = new DOMException("aborted", "AbortError");
  const other = new TypeError("fetch failed");
  assert.equal(isAbortError(abort), true);
  assert.equal(isAbortError(other), false);
  assert.equal(isAbortError(null), false);
  assert.equal(isAbortError(undefined), false);
});

// --- offline (ECONNREFUSED) ------------------------------------------------

test("ping: no listener classifies as bridge_offline (not bridge_timeout)", async () => {
  // Port 1 — nothing listening, ECONNREFUSED on connect. Must classify as
  // bridge_offline, NOT bridge_timeout.
  const client = new LiveClient(1);
  const result = await client.route("unreal_open_mcp_ping", {});
  assert.equal(result.isError, true);
  const body = bodyOf(result) as { error: { code: string; message: string } };
  assert.equal(body.error.code, "bridge_offline");
});

test("ping: offline error names the project lock path when projectPath is set", async () => {
  const client = new LiveClient(1, undefined, "/tmp/MyGame");
  const result = await client.route("unreal_open_mcp_ping", {});
  assert.equal(result.isError, true);
  const body = bodyOf(result) as { error: { code: string; message: string } };
  assert.equal(body.error.code, "bridge_offline");
  assert.match(
    body.error.message,
    /\.unreal-open-mcp\/instances\//,
    "offline hint must name the instance lock dir",
  );
});

test("ping: offline error falls back to a generic hint when projectPath is absent", async () => {
  const client = new LiveClient(1);
  const result = await client.route("unreal_open_mcp_ping", {});
  assert.equal(result.isError, true);
  const body = bodyOf(result) as { error: { code: string; message: string } };
  assert.equal(body.error.code, "bridge_offline");
  // Generic hint still mentions the env override so an agent/user can pin a port.
  assert.match(body.error.message, /UNREAL_OPEN_MCP_BRIDGE_PORT/);
});

// --- HTTP 500 --------------------------------------------------------------

test("ping: HTTP 500 surfaces as bridge_http_error", async () => {
  const bridge = await startBridgeStub((_req, res) => {
    res.writeHead(500, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ error: { code: "internal", message: "boom" } }));
  });
  try {
    const client = new LiveClient(bridge.port);
    const result = await client.route("unreal_open_mcp_ping", {});
    assert.equal(result.isError, true);
    const body = bodyOf(result) as {
      error: { code: string; message: string };
    };
    assert.equal(body.error.code, "internal");
    assert.equal(body.error.message, "boom");
  } finally {
    await bridge.close();
  }
});

// --- malformed body --------------------------------------------------------

test("ping: 200 with non-JSON body classifies as bridge_response_unparsable", async () => {
  const bridge = await startBridgeStub((_req, res) => {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end("<<<not json>>>");
  });
  try {
    const client = new LiveClient(bridge.port);
    const result = await client.route("unreal_open_mcp_ping", {});
    assert.equal(result.isError, true);
    const body = bodyOf(result) as { error: { code: string } };
    assert.equal(body.error.code, "bridge_response_unparsable");
  } finally {
    await bridge.close();
  }
});

// --- non-ping route --------------------------------------------------------

test("route: a non-ping tool name returns tool_not_routed (no HTTP)", async () => {
  // No bridge stub at all — the call must not attempt HTTP because the bridge
  // has no tool-dispatch endpoint yet. A tool_not_routed result returns
  // synchronously without touching the network.
  const client = new LiveClient(1);
  const result = await client.route("unreal_open_mcp_some_future_tool", { foo: 1 });
  assert.equal(result.isError, true);
  const body = bodyOf(result) as { error: { code: string; message: string } };
  assert.equal(body.error.code, "tool_not_routed");
  assert.match(
    body.error.message,
    /unreal_open_mcp_some_future_tool/,
    "message names the tool that was not routed",
  );
});

// --- bearer token header ---------------------------------------------------

/**
 * Handler that records the Authorization header of the first /ping request it
 * sees, then responds with the healthy body. Asserts LiveClient attaches the
 * bearer token discovered from the instance lock.
 */
function headerCapturingHandler(
  seen: { auth?: string | null },
): (req: IncomingMessage, res: ServerResponse) => void {
  return (req, res) => {
    if (req.url === "/ping" && seen.auth === undefined) {
      seen.auth = req.headers["authorization"] ?? null;
    }
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(HEALTHY_PING));
  };
}

test("ping: sends Authorization: Bearer <token> when a token was provided", async () => {
  const seen: { auth?: string | null } = {};
  const bridge = await startBridgeStub(headerCapturingHandler(seen));
  try {
    const token = "deadbeef".repeat(8);
    const client = new LiveClient(bridge.port, token);
    await client.route("unreal_open_mcp_ping", {});
    assert.equal(seen.auth, `Bearer ${token}`);
  } finally {
    await bridge.close();
  }
});

test("ping: omits Authorization header when no token was provided", async () => {
  const seen: { auth?: string | null } = {};
  const bridge = await startBridgeStub(headerCapturingHandler(seen));
  try {
    const client = new LiveClient(bridge.port);
    await client.route("unreal_open_mcp_ping", {});
    assert.equal(seen.auth, null);
  } finally {
    await bridge.close();
  }
});
