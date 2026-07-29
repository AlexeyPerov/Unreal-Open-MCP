// Unit tests for the ToolRouter dispatch spine (P8.6).
//
// Covers the route-policy classification (live vs local vs offline vs batch),
// the live delegation + route-metadata stamping, the local short-circuit
// (capabilities resolves with the editor down), and the offline/batch
// structured refusals. The local bridge_status handler is exercised end-to-end
// in tools/bridge-status.test.ts via the `handleLocalTool` facade; here we
// focus on the router's classification + metadata contract.

import test from "node:test";
import assert from "node:assert/strict";

import {
  ToolRouter,
  routePolicy,
  type Route,
  type Router,
} from "./tool-router.js";
import type { CallToolResult } from "@modelcontextprotocol/sdk/types.js";

/** Parse the first text content block of a CallToolResult as JSON. */
function bodyOf(result: CallToolResult): Record<string, unknown> {
  const block = result.content[0];
  assert.ok(block?.type === "text", "first content block must be text");
  assert.ok(typeof block.text === "string");
  return JSON.parse(block.text);
}

/** Build a stub live transport that records every call and returns a fixed body. */
function makeStubLive(
  body: unknown,
): Router & { calls: Array<{ tool: string; args: Record<string, unknown> }> } {
  const calls: Array<{ tool: string; args: Record<string, unknown> }> = [];
  return {
    calls,
    async route(tool: string, args: Record<string, unknown>) {
      calls.push({ tool, args });
      return {
        content: [{ type: "text", text: JSON.stringify(body) }],
        isError: false,
      };
    },
  };
}

// ---------------------------------------------------------------------------
// routePolicy — the classification table
// ---------------------------------------------------------------------------

test("routePolicy classifies the local-route tools as local", () => {
  assert.equal(routePolicy("unreal_open_mcp_capabilities"), "local");
  assert.equal(routePolicy("unreal_open_mcp_bridge_status"), "local");
});

test("routePolicy defaults every other tool to live", () => {
  // A representative slice across phases: ping (P1.7), actor_find (P2.2),
  // level_get_data (P2.7), asset_find (P4.1), blueprint_compile (P6.4),
  // source_compile (P7.4). None are in the offline/local/batch sets today.
  const liveTools = [
    "unreal_open_mcp_ping",
    "unreal_open_mcp_actor_find",
    "unreal_open_mcp_level_get_data",
    "unreal_open_mcp_asset_find",
    "unreal_open_mcp_blueprint_compile",
    "unreal_open_mcp_source_compile",
  ];
  for (const name of liveTools) {
    assert.equal(routePolicy(name), "live", `${name} should route live`);
  }
});

test("routePolicy defaults an unknown tool name to live (not error)", () => {
  // An unregistered name still classifies live — the router delegates to the
  // transport, which returns the bridge's tool_not_found. Classification never
  // throws; the policy table is a name → route map, not a registry gate.
  assert.equal(routePolicy("unreal_open_mcp_does_not_exist"), "live");
});

// ---------------------------------------------------------------------------
// Live route — delegates to the transport + stamps metadata
// ---------------------------------------------------------------------------

test("live route delegates to the transport and stamps _source=live + _route.route=live", async () => {
  const live = makeStubLive({ connected: true, status: "ready" });
  const router = new ToolRouter(live, null);
  const result = await router.route("unreal_open_mcp_ping", {});

  assert.deepEqual(live.calls, [{ tool: "unreal_open_mcp_ping", args: {} }]);
  assert.equal(result.isError, false);
  const body = bodyOf(result);
  assert.equal(body.connected, true);
  assert.equal(body._source, "live");
  assert.deepEqual(body._route, { route: "live" });
});

test("live route forwards args verbatim to the transport", async () => {
  const live = makeStubLive({ ok: true });
  const router = new ToolRouter(live, null);
  await router.route("unreal_open_mcp_actor_find", {
    filter: "PointLight",
    limit: 10,
  });
  assert.deepEqual(live.calls[0].args, { filter: "PointLight", limit: 10 });
});

test("live route stamps metadata even when the transport returns an error body", async () => {
  // A bridge-returned tool error (ok:false envelope) still carries route
  // metadata so agent recovery logic can trust the field on every response.
  const live: Router = {
    async route() {
      return {
        content: [
          {
            type: "text",
            text: JSON.stringify({
              error: { code: "invalid_request", message: "bad args" },
            }),
          },
        ],
        isError: true,
      };
    },
  };
  const router = new ToolRouter(live, null);
  const result = await router.route("unreal_open_mcp_actor_find", {});
  assert.equal(result.isError, true);
  const body = bodyOf(result);
  assert.equal(body._source, "live");
  assert.deepEqual(body._route, { route: "live" });
  assert.equal(
    (body.error as { code: string }).code,
    "invalid_request",
  );
});

test("live route is a no-op stamp when the transport body is not JSON (preserves isError)", async () => {
  // A non-JSON text block (or an MCP image-only result) is left untouched —
  // tagResult never throws on an unparsable body.
  const live: Router = {
    async route() {
      return {
        content: [{ type: "text", text: "not-json" }],
        isError: false,
      };
    },
  };
  const router = new ToolRouter(live, null);
  const result = await router.route("unreal_open_mcp_ping", {});
  assert.equal(result.isError, false);
  assert.equal(
    (result.content[0] as { text: string }).text,
    "not-json",
  );
});

// ---------------------------------------------------------------------------
// Local route — short-circuits the transport, works with the editor down
// ---------------------------------------------------------------------------

test("local route never hits the transport (capabilities resolves in-process)", async () => {
  const live = makeStubLive({ shouldNotReach: true });
  const router = new ToolRouter(live, null);
  const result = await router.route("unreal_open_mcp_capabilities", {});

  assert.deepEqual(live.calls, [], "capabilities must NOT route through the transport");
  assert.equal(result.isError, false);
  const body = bodyOf(result);
  assert.equal(body._source, "local");
  assert.deepEqual(body._route, { route: "local" });
  // The capability surface is populated from the in-memory tool list.
  assert.ok(Array.isArray(body.tools) && (body.tools as unknown[]).length > 0);
});

test("local route honors the capabilities kind=rules filter", async () => {
  const live = makeStubLive({});
  const router = new ToolRouter(live, null);
  const result = await router.route("unreal_open_mcp_capabilities", {
    kind: "rules",
  });
  const body = bodyOf(result);
  assert.equal((body.tools as unknown[]).length, 0);
  assert.ok((body.rules as unknown[]).length > 0);
  assert.equal((body.fixes as unknown[]).length, 0);
});

test("local route bridge_status fires one ping probe through the transport", async () => {
  // bridge_status is local-route but composes a /ping probe via the live
  // transport. With a reachable probe the status derives "running".
  const live = makeStubLive({
    connected: true,
    compiling: false,
    isPlaying: false,
  });
  const router = new ToolRouter(live, null);
  const result = await router.route("unreal_open_mcp_bridge_status", {});

  assert.deepEqual(
    live.calls.map((c) => c.tool),
    ["unreal_open_mcp_ping"],
    "bridge_status must fire exactly one ping probe",
  );
  assert.equal(result.isError, false);
  const body = bodyOf(result);
  assert.equal(body._source, "local");
  assert.deepEqual(body._route, { route: "local" });
  assert.equal(body.status, "running");
});

test("local route bridge_status with a failing probe still reports a status (not an error)", async () => {
  // A dead/stopped bridge is a *successful* status read — bridge_status never
  // returns isError:true. With no project path + a failing probe the lock is
  // absent (classification "gone") and the status is "stopped".
  const live: Router = {
    async route() {
      return {
        content: [
          { type: "text", text: JSON.stringify({ error: { code: "bridge_offline" } }) },
        ],
        isError: true,
      };
    },
  };
  const router = new ToolRouter(live, null);
  const result = await router.route("unreal_open_mcp_bridge_status", {});
  assert.equal(result.isError, false);
  const body = bodyOf(result);
  assert.equal(body.status, "stopped");
  assert.equal(body._source, "local");
});

// ---------------------------------------------------------------------------
// Offline — P8.7 wired end-to-end (batch still refuses until its handler lands)
// ---------------------------------------------------------------------------

test("routePolicy classifies the three P8.7 offline tools as offline", () => {
  // P8.7 landed the first offline tools. Each is resolved from disk and never
  // hits the live transport. The behavioral coverage (route metadata + the
  // never-hit-live contract + structured diagnostics) lives in
  // offline/offline.test.ts; this pins the classification table.
  assert.equal(routePolicy("unreal_open_mcp_read_compile_errors"), "offline");
  assert.equal(routePolicy("unreal_open_mcp_source_read_offline"), "offline");
  assert.equal(routePolicy("unreal_open_mcp_project_index"), "offline");
  // The two local-route tools are explicitly NOT offline.
  assert.equal(routePolicy("unreal_open_mcp_capabilities"), "local");
  assert.equal(routePolicy("unreal_open_mcp_bridge_status"), "local");
  // An unrelated name still defaults to live.
  assert.equal(routePolicy("unreal_open_mcp_ping"), "live");
});

test("routePolicy has no batch tools today (commandlet deferred)", () => {
  // compile_check / scan_all will route batch once the commandlet spawn lands.
  // Until then they classify live so the bridge's own tool_not_found surfaces
  // honestly rather than a premature batch_not_implemented.
  assert.equal(routePolicy("unreal_open_mcp_compile_check"), "live");
  assert.equal(routePolicy("unreal_open_mcp_scan_all"), "live");
});

// ---------------------------------------------------------------------------
// Route type coverage — every Route value is reachable through routePolicy
// ---------------------------------------------------------------------------

test("every Route value is represented in the policy table (live + local + offline today)", () => {
  const seen = new Set<Route>();
  seen.add(routePolicy("unreal_open_mcp_ping")); // live
  seen.add(routePolicy("unreal_open_mcp_capabilities")); // local
  seen.add(routePolicy("unreal_open_mcp_read_compile_errors")); // offline (P8.7)
  // batch is empty today; documented as planned. This assertion pins the three
  // wired routes so a regression that re-classifies either is caught.
  assert.ok(seen.has("live"));
  assert.ok(seen.has("local"));
  assert.ok(seen.has("offline"));
});
