// manage_tools (P8.10) — tool definition + router-handler tests.
//
// The definition lives in manage-tools.ts (name / description / inputSchema
// only — the handler is a method on ToolRouter). These tests pin:
//   1. The tool definition / registration / schema contract (so a schema edit
//      is caught), mirroring the capabilities.test.ts pattern.
//   2. The router handler's four actions (list_groups / activate / deactivate /
//      reset), the three error codes (missing_parameter / unknown_action /
//      unknown_group), the session-state_not_wired guard, and — the load-bearing
//      P8.10 contract — that the onToolListChanged callback fires exactly once
//      per visibility change and NOT at all for a no-op mutation.
//
// Copied from Unity Open MCP's routeManageTools contract (copy fidelity for the
// four core actions). Unity's suggest / activate_for actions are deferred; the
// v1 surface tested here is list_groups / activate / deactivate / reset only.

import test from "node:test";
import assert from "node:assert/strict";

import { manageTools } from "./manage-tools.js";
import { ALL_TOOLS } from "./index.js";
import { ToolRouter, routePolicy, type Router } from "../tool-router.js";
import { ToolSessionState, filterVisibleTools } from "../tool-session-state.js";
import type { CallToolResult } from "@modelcontextprotocol/sdk/types.js";

// ---------------------------------------------------------------------------
// Tool definition / registration
// ---------------------------------------------------------------------------

test("manage_tools tool is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(manageTools.name, "unreal_open_mcp_manage_tools");
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_manage_tools"),
    "manage_tools must be in ALL_TOOLS",
  );
});

test("manage_tools is classified local-route (no bridge round-trip)", () => {
  assert.equal(routePolicy("unreal_open_mcp_manage_tools"), "local");
});

test("manage_tools schema requires action + exposes the four core actions + optional group", () => {
  const schema = manageTools.inputSchema as {
    type: string;
    properties: {
      action: { enum?: string[] };
      group: { type?: string };
    };
    required?: string[];
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  assert.deepEqual(schema.properties.action.enum, [
    "list_groups",
    "activate",
    "deactivate",
    "reset",
  ]);
  assert.equal(schema.properties.group.type, "string");
  assert.deepEqual(schema.required, ["action"]);
  assert.equal(schema.additionalProperties, false);
});

test("manage_tools description documents the local route + error vocabulary + list_changed", () => {
  const desc = manageTools.description ?? "";
  assert.match(desc, /local/i, "must document the local route");
  assert.match(desc, /missing_parameter/);
  assert.match(desc, /unknown_action/);
  assert.match(desc, /unknown_group/);
  assert.match(desc, /list_changed/);
});

// ---------------------------------------------------------------------------
// Router-handler test harness
// ---------------------------------------------------------------------------

/** Parse the first text content block of a CallToolResult as JSON. */
function bodyOf(result: CallToolResult): Record<string, unknown> {
  const block = result.content[0];
  assert.ok(block?.type === "text", "first content block must be text");
  assert.ok(typeof block.text === "string");
  return JSON.parse(block.text);
}

/** Strip the _source / _route envelope so a deepEqual targets the inner body. */
function payloadOf(result: CallToolResult): Record<string, unknown> {
  const { _source: _s, _route: _r, ...payload } = bodyOf(result);
  void _s;
  void _r;
  return payload;
}

/** A live transport stub that records calls; manage_tools never reaches it. */
function makeStubLive(): Router & {
  calls: Array<{ tool: string }>;
} {
  const calls: Array<{ tool: string }> = [];
  return {
    calls,
    async route(tool: string) {
      calls.push({ tool });
      return { content: [{ type: "text", text: "{}" }], isError: false };
    },
  };
}

/**
 * Build a router wired to a FRESH session state + a notification spy. The spy
 * records every onToolListChanged invocation so a test can assert the exact
 * fire count (the no-op-suppression contract). Each test gets its own session
 * store so cases are isolated.
 */
function makeManageRouter() {
  const session = new ToolSessionState();
  const fired: number[] = [];
  const router = new ToolRouter(makeStubLive(), null, session, () => {
    fired.push(1);
  });
  return { router, session, fired };
}

async function callManage(
  router: ToolRouter,
  args: Record<string, unknown>,
): Promise<CallToolResult> {
  return router.route("unreal_open_mcp_manage_tools", args);
}

// ---------------------------------------------------------------------------
// list_groups — read-only enumeration
// ---------------------------------------------------------------------------

test("list_groups returns every group with active flag, default flag, activation source, and roster", async () => {
  const { router, fired } = makeManageRouter();
  const result = await callManage(router, { action: "list_groups" });
  assert.equal(result.isError, false);
  assert.equal(fired.length, 0, "list_groups is read-only — must not notify");
  const body = bodyOf(result);
  assert.deepEqual(body._source, "local");
  assert.deepEqual(body._route, { route: "local" });
  const payload = payloadOf(result) as {
    groups: Array<{
      id: string;
      active: boolean;
      defaultEnabled: boolean;
      activationSource: string | null;
      tools: string[];
    }>;
    activeGroups: string[];
  };
  assert.deepEqual(
    payload.groups.map((g) => g.id),
    ["core", "gate-and-verify", "typed-editor", "diagnostics"],
    "catalog order is preserved",
  );
  const core = payload.groups.find((g) => g.id === "core")!;
  assert.equal(core.active, true);
  assert.equal(core.defaultEnabled, true);
  assert.equal(core.activationSource, "default");
  assert.ok(core.tools.includes("unreal_open_mcp_ping"));
  const typed = payload.groups.find((g) => g.id === "typed-editor")!;
  assert.equal(typed.active, false);
  assert.equal(typed.defaultEnabled, false);
  assert.ok(
    typed.tools.includes("unreal_open_mcp_actor_find"),
    "typed-editor roster must list actor_find",
  );
  assert.deepEqual(payload.activeGroups, ["core"]);
});

// ---------------------------------------------------------------------------
// activate / deactivate — mutation + notification gating
// ---------------------------------------------------------------------------

test("activate typed-editor mutates session state, returns changed:true, and fires list_changed once", async () => {
  const { router, session, fired } = makeManageRouter();
  const result = await callManage(router, {
    action: "activate",
    group: "typed-editor",
  });
  assert.equal(result.isError, false);
  const payload = payloadOf(result) as {
    action: string;
    group: string;
    changed: boolean;
    activeGroups: string[];
  };
  assert.equal(payload.changed, true);
  assert.deepEqual(payload.activeGroups, ["core", "typed-editor"]);
  assert.ok(session.isGroupActive("typed-editor"), "session state must reflect the activation");
  assert.equal(fired.length, 1, "a real visibility change fires exactly one notification");
});

test("activate an already-active group is a no-op: changed:false and NO notification", async () => {
  const { router, fired } = makeManageRouter();
  await callManage(router, { action: "activate", group: "typed-editor" });
  assert.equal(fired.length, 1);
  // Second activate of the same group is a no-op.
  const result = await callManage(router, {
    action: "activate",
    group: "typed-editor",
  });
  const payload = payloadOf(result) as { changed: boolean };
  assert.equal(payload.changed, false);
  assert.equal(fired.length, 1, "a no-op activate must NOT fire a notification");
});

test("deactivate a previously-activated group mutates state and fires list_changed once", async () => {
  const { router, session, fired } = makeManageRouter();
  await callManage(router, { action: "activate", group: "gate-and-verify" });
  assert.equal(fired.length, 1);
  const result = await callManage(router, {
    action: "deactivate",
    group: "gate-and-verify",
  });
  const payload = payloadOf(result) as { changed: boolean; activeGroups: string[] };
  assert.equal(payload.changed, true);
  assert.deepEqual(payload.activeGroups, ["core"]);
  assert.equal(session.isGroupActive("gate-and-verify"), false);
  assert.equal(fired.length, 2, "the deactivate change fires a second notification");
});

test("deactivate an inactive group is a no-op: changed:false and NO notification", async () => {
  const { router, fired } = makeManageRouter();
  // gate-and-verify is off by default — deactivating it changes nothing.
  const result = await callManage(router, {
    action: "deactivate",
    group: "gate-and-verify",
  });
  const payload = payloadOf(result) as { changed: boolean };
  assert.equal(payload.changed, false);
  assert.equal(fired.length, 0, "a no-op deactivate must NOT fire a notification");
});

// ---------------------------------------------------------------------------
// reset — restore default-on set + notification gating
// ---------------------------------------------------------------------------

test("reset restores the default-on set and fires list_changed only when the surface actually changes", async () => {
  const { router, session, fired } = makeManageRouter();
  await callManage(router, { action: "activate", group: "typed-editor" });
  assert.equal(fired.length, 1);
  assert.equal(session.isGroupActive("typed-editor"), true);

  const result = await callManage(router, { action: "reset" });
  assert.equal(result.isError, false);
  const payload = payloadOf(result) as { reset: boolean; activeGroups: string[] };
  assert.equal(payload.reset, true);
  assert.deepEqual(payload.activeGroups, ["core"]);
  assert.equal(session.isGroupActive("typed-editor"), false);
  assert.equal(fired.length, 2, "reset that shrinks the surface fires one notification");
});

test("reset of an already-default session is a no-op: NO notification", async () => {
  const { router, fired } = makeManageRouter();
  // Fresh session is already the default-on set — reset changes nothing.
  const result = await callManage(router, { action: "reset" });
  assert.equal(result.isError, false);
  assert.equal(fired.length, 0, "a no-op reset must NOT fire a notification");
});

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

test("activate without group returns missing_parameter", async () => {
  const { router, fired } = makeManageRouter();
  const result = await callManage(router, { action: "activate" });
  assert.equal(result.isError, true);
  const body = bodyOf(result) as { error: { code: string; message: string } };
  assert.equal(body.error.code, "missing_parameter");
  assert.equal(fired.length, 0, "an error must never notify");
});

test("missing action returns missing_parameter", async () => {
  const { router } = makeManageRouter();
  const result = await callManage(router, {});
  assert.equal(result.isError, true);
  const body = bodyOf(result) as { error: { code: string } };
  assert.equal(body.error.code, "missing_parameter");
});

test("unknown group returns unknown_group with the valid id list", async () => {
  const { router, fired } = makeManageRouter();
  const result = await callManage(router, {
    action: "activate",
    group: "no-such-group",
  });
  assert.equal(result.isError, true);
  const body = bodyOf(result) as { error: { code: string; message: string } };
  assert.equal(body.error.code, "unknown_group");
  assert.match(body.error.message, /core/);
  assert.match(body.error.message, /typed-editor/);
  assert.equal(fired.length, 0);
});

test("unknown action returns unknown_action", async () => {
  const { router } = makeManageRouter();
  const result = await callManage(router, { action: "suggest" });
  assert.equal(result.isError, true);
  const body = bodyOf(result) as { error: { code: string; message: string } };
  assert.equal(body.error.code, "unknown_action");
  // suggest / activate_for are the deferred Unity actions — point the agent at
  // the four that ARE supported.
  assert.match(body.error.message, /list_groups/);
  assert.match(body.error.message, /activate/);
  assert.match(body.error.message, /deactivate/);
  assert.match(body.error.message, /reset/);
});

test("manage_tools against a router without a session store returns session_state_not_wired (never crashes)", async () => {
  // Legacy `new ToolRouter(live, path)` call sites that never invoke
  // manage_tools must not crash if they do. No session, no callback.
  const router = new ToolRouter(makeStubLive(), null);
  const result = await callManage(router, { action: "list_groups" });
  assert.equal(result.isError, true);
  const body = bodyOf(result) as { error: { code: string } };
  assert.equal(body.error.code, "session_state_not_wired");
});

// ---------------------------------------------------------------------------
// Full loop: activate → list reflects it → deactivate → list shrinks
// ---------------------------------------------------------------------------

test("activate makes a previously-hidden typed-editor tool visible in ListTools, deactivate hides it again", async () => {
  // This mirrors the handleListTools filter using the same session instance the
  // router mutated, proving the store is the shared seam between manage_tools
  // and tools/list.
  const { router, session } = makeManageRouter();
  const visibleNames = () =>
    filterVisibleTools(ALL_TOOLS, session).map((t) => t.name);

  assert.ok(
    !visibleNames().includes("unreal_open_mcp_actor_find"),
    "actor_find must be hidden before typed-editor is active",
  );
  await callManage(router, { action: "activate", group: "typed-editor" });
  assert.ok(
    visibleNames().includes("unreal_open_mcp_actor_find"),
    "actor_find must be visible after typed-editor activates",
  );
  await callManage(router, { action: "deactivate", group: "typed-editor" });
  assert.ok(
    !visibleNames().includes("unreal_open_mcp_actor_find"),
    "actor_find must be hidden again after typed-editor deactivates",
  );
});
