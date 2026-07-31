// Tests for the per-session tool-group visibility store + filter.
//
// Pins the contract manage_tools (P8.10) will build on, and the ListTools
// filtering wired in P8.9: a fresh session advertises `core` (+ always-visible
// meta / recovery tools), activation reveals a group's tools, deactivation
// hides them while always-visible tools survive, and reset restores defaults.

import test from "node:test";
import assert from "node:assert/strict";

import type { Tool } from "@modelcontextprotocol/sdk/types.js";

import { DEFAULT_ENABLED_GROUPS } from "./capabilities/tool-groups.js";
import {
  ToolSessionState,
  filterVisibleTools,
} from "./tool-session-state.js";
import { ALL_TOOLS } from "./tools/index.js";

// ---------------------------------------------------------------------------
// Fresh-session defaults
// ---------------------------------------------------------------------------

test("a fresh session activates only the default-on groups", () => {
  const state = new ToolSessionState();
  assert.deepEqual(state.activeGroups(), Array.from(DEFAULT_ENABLED_GROUPS).sort());
});

test("a fresh session reports core as default-on and others as inactive", () => {
  const state = new ToolSessionState();
  assert.equal(state.isGroupActive("core"), true);
  assert.equal(state.isGroupActive("gate-and-verify"), false);
  assert.equal(state.isGroupActive("typed-editor"), false);
  assert.equal(state.isGroupActive("diagnostics"), false);
});

test("default-on groups report activationSource 'default'", () => {
  const state = new ToolSessionState();
  assert.equal(state.activationSource("core"), "default");
  // An opt-in group that was never touched reports null (initial state).
  assert.equal(state.activationSource("typed-editor"), null);
});

// ---------------------------------------------------------------------------
// activate / deactivate
// ---------------------------------------------------------------------------

test("activate reveals an opt-in group and flips its source to manual", () => {
  const state = new ToolSessionState();
  assert.equal(state.activate("typed-editor"), true);
  assert.equal(state.isGroupActive("typed-editor"), true);
  assert.equal(state.activationSource("typed-editor"), "manual");
});

test("activate is a no-op (returns false) when the group is already active", () => {
  const state = new ToolSessionState();
  // core is default-on → activating it again is a no-op.
  assert.equal(state.activate("core"), false);
  assert.equal(state.isGroupActive("core"), true);
  // source stays 'default' — a no-op does not rewrite intent.
  assert.equal(state.activationSource("core"), "default");
});

test("activate rejects an unknown group id", () => {
  const state = new ToolSessionState();
  assert.equal(state.activate("nope"), false);
  assert.equal(state.isGroupActive("nope"), false);
});

test("a manually-activated then re-activated group stays manual", () => {
  const state = new ToolSessionState();
  state.activate("gate-and-verify");
  // Second activate is a no-op (already active); source must remain 'manual'.
  assert.equal(state.activate("gate-and-verify"), false);
  assert.equal(state.activationSource("gate-and-verify"), "manual");
});

test("deactivate hides an active group and clears its source", () => {
  const state = new ToolSessionState();
  state.activate("typed-editor");
  assert.equal(state.deactivate("typed-editor"), true);
  assert.equal(state.isGroupActive("typed-editor"), false);
  assert.equal(state.activationSource("typed-editor"), null);
});

test("deactivate is a no-op (returns false) when the group is already inactive", () => {
  const state = new ToolSessionState();
  assert.equal(state.deactivate("typed-editor"), false);
});

test("deactivate rejects an unknown group id", () => {
  const state = new ToolSessionState();
  assert.equal(state.deactivate("nope"), false);
});

test("deactivating core is allowed (always-visible tools keep the session usable)", () => {
  const state = new ToolSessionState();
  assert.equal(state.deactivate("core"), true);
  assert.equal(state.isGroupActive("core"), false);
  // ping is in core's group roster but is always-visible — it survives the
  // teardown (asserted in the filter tests below).
});

test("activate after deactivate re-reveals the group as manual", () => {
  const state = new ToolSessionState();
  state.activate("typed-editor");
  state.deactivate("typed-editor");
  assert.equal(state.activationSource("typed-editor"), null);
  assert.equal(state.activate("typed-editor"), true);
  assert.equal(state.activationSource("typed-editor"), "manual");
});

// ---------------------------------------------------------------------------
// reset
// ---------------------------------------------------------------------------

test("reset restores the default-on set after mutations", () => {
  const state = new ToolSessionState();
  state.activate("typed-editor");
  state.deactivate("core");
  assert.equal(state.reset(), true);
  assert.deepEqual(
    state.activeGroups(),
    Array.from(DEFAULT_ENABLED_GROUPS).sort(),
  );
  assert.equal(state.isGroupActive("typed-editor"), false);
  assert.equal(state.isGroupActive("core"), true);
  assert.equal(state.activationSource("core"), "default");
  assert.equal(state.activationSource("typed-editor"), null);
});

// ---------------------------------------------------------------------------
// filterVisibleTools — fresh session + activation
// ---------------------------------------------------------------------------

test("a fresh session shows only core tools + always-visible meta/recovery tools", () => {
  const state = new ToolSessionState();
  const visible = filterVisibleTools(ALL_TOOLS, state);
  assert.ok(
    visible.length < ALL_TOOLS.length,
    "fresh session must shrink the surface (default surface is lean)",
  );
  // ping is the one core tool that is not already always-visible-meta; it must
  // show up. The always-visible recovery tools must all survive.
  const names = visible.map((t) => t.name);
  assert.ok(names.includes("unreal_open_mcp_ping"));
  assert.ok(names.includes("unreal_open_mcp_capabilities"));
  assert.ok(names.includes("unreal_open_mcp_bridge_status"));
  assert.ok(names.includes("unreal_open_mcp_read_compile_errors"));
});

test("activating typed-editor reveals actor tools (and keeps the default surface)", () => {
  const state = new ToolSessionState();
  const before = filterVisibleTools(ALL_TOOLS, state);
  assert.ok(
    !before.map((t) => t.name).includes("unreal_open_mcp_actor_find"),
    "actor_find must be hidden before typed-editor is activated",
  );
  state.activate("typed-editor");
  const after = filterVisibleTools(ALL_TOOLS, state);
  const names = after.map((t) => t.name);
  assert.ok(names.includes("unreal_open_mcp_actor_find"));
  assert.ok(names.includes("unreal_open_mcp_blueprint_compile"));
  assert.ok(names.includes("unreal_open_mcp_source_compile"));
  assert.ok(after.length > before.length, "activation must grow the surface");
});

test("activating gate-and-verify reveals the verify surface", () => {
  const state = new ToolSessionState();
  const before = filterVisibleTools(ALL_TOOLS, state).map((t) => t.name);
  assert.ok(!before.includes("unreal_open_mcp_validate_edit"));
  state.activate("gate-and-verify");
  const after = filterVisibleTools(ALL_TOOLS, state).map((t) => t.name);
  for (const name of [
    "unreal_open_mcp_validate_edit",
    "unreal_open_mcp_checkpoint_create",
    "unreal_open_mcp_delta",
    "unreal_open_mcp_apply_fix",
  ]) {
    assert.ok(after.includes(name), `${name} must be visible after activating gate-and-verify`);
  }
});

test("deactivating core still leaves always-visible ping / bridge_status / capabilities", () => {
  const state = new ToolSessionState();
  state.deactivate("core");
  const names = filterVisibleTools(ALL_TOOLS, state).map((t) => t.name);
  for (const name of [
    "unreal_open_mcp_ping",
    "unreal_open_mcp_bridge_status",
    "unreal_open_mcp_capabilities",
  ]) {
    assert.ok(
      names.includes(name),
      `${name} must survive a core teardown (always-visible)`,
    );
  }
});

test("null-group tools are always visible even with every group torn down", () => {
  // Tear down core (the only default-on group). The always-visible allow-list
  // + the null-group fallback both keep the meta / recovery surface visible.
  const state = new ToolSessionState();
  state.deactivate("core");
  const names = filterVisibleTools(ALL_TOOLS, state).map((t) => t.name);
  for (const name of [
    "unreal_open_mcp_read_compile_errors",
    "unreal_open_mcp_source_read_offline",
    "unreal_open_mcp_project_index",
  ]) {
    assert.ok(names.includes(name), `${name} must be visible (null group)`);
  }
});

test("reset shrinks an expanded session back to the default surface", () => {
  const state = new ToolSessionState();
  state.activate("typed-editor");
  state.activate("gate-and-verify");
  const expanded = filterVisibleTools(ALL_TOOLS, state);
  state.reset();
  const reset = filterVisibleTools(ALL_TOOLS, state);
  assert.ok(reset.length < expanded.length);
  // The reset surface must equal a fresh-session surface.
  const fresh = filterVisibleTools(ALL_TOOLS, new ToolSessionState());
  assert.deepEqual(
    reset.map((t) => t.name),
    fresh.map((t) => t.name),
  );
});

// ---------------------------------------------------------------------------
// filterVisibleTools — resolver injection + custom fixtures
// ---------------------------------------------------------------------------

test("filterVisibleTools honors a custom group resolver", () => {
  // Proves the resolveGroup seam lets tests swap grouping without touching the
  // catalog. A tool mapped to an active group by the custom resolver is kept.
  const state = new ToolSessionState();
  const tools: Tool[] = [
    { name: "x_one", description: "", inputSchema: { type: "object", properties: {} } },
    { name: "x_two", description: "", inputSchema: { type: "object", properties: {} } },
  ];
  const visible = filterVisibleTools(
    tools,
    state,
    (name) => (name === "x_one" ? "core" : null),
  );
  assert.deepEqual(
    visible.map((t) => t.name),
    ["x_one", "x_two"],
  );
});

test("filterVisibleTools drops a tool whose group is inactive under a custom resolver", () => {
  const state = new ToolSessionState();
  const tools: Tool[] = [
    { name: "x_one", description: "", inputSchema: { type: "object", properties: {} } },
  ];
  const visible = filterVisibleTools(
    tools,
    state,
    () => "typed-editor", // inactive group
  );
  assert.equal(visible.length, 0);
});
