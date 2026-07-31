// Tests for the canonical tool-group catalog and per-tool group assignment.
//
// Pins the catalog invariants, the lean default-on set (roadmap P8.9 pin:
// `core` only), and the exhaustive groupFor coverage over the real registered
// tool set. The exhaustive parity test is the safeguard against a new tool
// being added to tools/index.ts without a group assignment — every registered
// tool must map to a known group OR explicitly be `null` (the meta / offline
// recovery surface).

import test from "node:test";
import assert from "node:assert/strict";

import {
  TOOL_GROUPS,
  DEFAULT_ENABLED_GROUPS,
  GROUP_IDS,
  getGroup,
  groupFor,
  groupToTools,
} from "./tool-groups.js";
import { ALL_TOOLS } from "../tools/index.js";

// ---------------------------------------------------------------------------
// Catalog invariants
// ---------------------------------------------------------------------------

test("TOOL_GROUPS has stable, unique, lowercase ids", () => {
  const ids = TOOL_GROUPS.map((g) => g.id);
  assert.equal(new Set(ids).size, ids.length, "group ids must be unique");
  for (const id of ids) {
    assert.equal(id, id.toLowerCase(), `${id} must be lowercase`);
    assert.ok(/^[a-z][a-z0-9-]*$/.test(id), `${id} must be a valid group id`);
  }
});

test("every group carries a non-empty description", () => {
  for (const g of TOOL_GROUPS) {
    assert.ok(
      typeof g.description === "string" && g.description.length > 10,
      `${g.id} must have a meaningful description`,
    );
  }
});

test("DEFAULT_ENABLED_GROUPS matches the catalog's defaultEnabled entries", () => {
  // Single source of truth: the set of groups marked `defaultEnabled: true`
  // in the catalog. Asserting against the catalog keeps this test honest when
  // defaults change instead of hard-coding a snapshot.
  const expected = TOOL_GROUPS.filter((g) => g.defaultEnabled)
    .map((g) => g.id)
    .sort();
  assert.deepEqual(Array.from(DEFAULT_ENABLED_GROUPS).sort(), expected);
});

test("the lean default-on set is exactly core (roadmap P8.9 pin)", () => {
  // Session-ergonomics baseline: a fresh session advertises only `core` from
  // defaults (plus always-visible meta / recovery tools). Anything widening
  // the default surface is a regression — pin the exact id so a catalog edit
  // that re-enables a group is intentional. (Unity also defaults
  // gate-and-verify; the Unreal roadmap pin is core-only.)
  assert.deepEqual(Array.from(DEFAULT_ENABLED_GROUPS).sort(), ["core"]);
  // The other groups must NOT be default-on.
  for (const id of ["gate-and-verify", "typed-editor", "diagnostics"]) {
    assert.ok(
      !DEFAULT_ENABLED_GROUPS.has(id),
      `${id} must not be default-on (lean session surface)`,
    );
  }
});

test("GROUP_IDS matches TOOL_GROUPS ids", () => {
  assert.deepEqual(
    Array.from(GROUP_IDS).sort(),
    TOOL_GROUPS.map((g) => g.id).sort(),
  );
});

test("getGroup returns the catalog entry by id", () => {
  const core = getGroup("core");
  assert.ok(core);
  assert.equal(core!.defaultEnabled, true);
});

test("getGroup returns undefined for an unknown id", () => {
  assert.equal(getGroup("nope"), undefined);
});

test("diagnostics is the reserved empty group for the current phase", () => {
  // Profiler / per-frame diagnostic reads land here later. Kept in the catalog
  // so the id is stable before any tool maps to it.
  const diag = getGroup("diagnostics");
  assert.ok(diag);
  assert.equal(diag!.defaultEnabled, false);
  assert.deepEqual(groupToTools()["diagnostics"], []);
});

// ---------------------------------------------------------------------------
// groupFor — exhaustive parity over the real registered tool set
// ---------------------------------------------------------------------------

test("every registered tool maps to a known group or null (never an unknown group)", () => {
  // The safeguard against registry drift: a tool added to tools/index.ts
  // without a group assignment silently slips through. Every assignment must
  // point at a catalog group id; a typo would otherwise hide the tool under a
  // bogus group that is never activated.
  let assigned = 0;
  let alwaysVisible = 0;
  for (const tool of ALL_TOOLS) {
    const g = groupFor(tool.name);
    if (g === null) {
      alwaysVisible++;
      continue;
    }
    assert.ok(
      GROUP_IDS.has(g),
      `${tool.name} maps to unknown group '${g}'`,
    );
    assigned++;
  }
  assert.ok(assigned > 0, "at least one tool should be group-assigned");
  assert.ok(alwaysVisible > 0, "at least one tool should be always-visible");
});

test("groupFor returns null for the always-visible meta / recovery tools", () => {
  // These bypass the filter entirely — an agent must reach them even with
  // every group torn down.
  for (const name of [
    "unreal_open_mcp_capabilities",
    "unreal_open_mcp_bridge_status",
    "unreal_open_mcp_read_compile_errors",
    "unreal_open_mcp_source_read_offline",
    "unreal_open_mcp_project_index",
  ]) {
    assert.equal(groupFor(name), null, `${name} should be always-visible (null)`);
  }
});

test("ping is assigned to the core group", () => {
  // ping is both in the core group AND in the ALWAYS_VISIBLE_TOOLS allow-list
  // (the allow-list wins in filterVisibleTools). The group assignment is the
  // fallback that keeps ping discoverable in group rosters.
  assert.equal(groupFor("unreal_open_mcp_ping"), "core");
});

test("gate-and-verify group contains the verify surface", () => {
  const tools = groupToTools()["gate-and-verify"];
  assert.deepEqual(tools, [
    "unreal_open_mcp_apply_fix",
    "unreal_open_mcp_checkpoint_create",
    "unreal_open_mcp_delta",
    "unreal_open_mcp_validate_edit",
  ]);
});

test("typed-editor group contains the full authoring surface", () => {
  const tools = groupToTools()["typed-editor"];
  // Spot-check one tool per family — the exhaustive registry-parity test
  // above covers the full set.
  for (const name of [
    "unreal_open_mcp_actor_find",
    "unreal_open_mcp_level_open",
    "unreal_open_mcp_asset_find",
    "unreal_open_mcp_material_create",
    "unreal_open_mcp_blueprint_compile",
    "unreal_open_mcp_source_compile",
    "unreal_open_mcp_screenshot_viewport",
    "unreal_open_mcp_reflection_method_call",
  ]) {
    assert.ok(tools.includes(name), `${name} should be in typed-editor`);
  }
});

test("groupToTools is frozen / identical across calls", () => {
  // Computed ONCE at module load; every caller shares one reference.
  assert.strictEqual(groupToTools(), groupToTools());
});

test("every group in the catalog has an entry in groupToTools", () => {
  const map = groupToTools();
  for (const g of TOOL_GROUPS) {
    assert.ok(Array.isArray(map[g.id]), `${g.id} must have a roster entry`);
  }
});
