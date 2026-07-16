import test from "node:test";
import assert from "node:assert/strict";
import { levelOpen } from "./level-open.js";
import { levelSave } from "./level-save.js";
import { levelListLoaded } from "./level-list-loaded.js";
import { levelSetCurrent } from "./level-set-current.js";
import { levelUnloadSublevel } from "./level-unload-sublevel.js";
import { ALL_TOOLS } from "./index.js";

// The level lifecycle tool definitions are the catalog surface advertised via
// tools/list. P2.6 acceptance: all five tools are registered under the
// `unreal_open_mcp_level_` prefix; the four mutators mark themselves mutating
// and carry the forward-compat `paths_hint` + `gate` surface (deferred until
// P3.5); the read-only list tool carries no gate; every description tells an
// agent the gate deferral and the family's shared error codes. Mirrors the
// one-test-per-tool actor catalog tests, folded into a single file per the
// P2.6 plan.

// ---------------------------------------------------------------------------
// Shared shape helpers.
// ---------------------------------------------------------------------------

type MutatingSchema = {
  type: string;
  required: string[];
  properties: Record<
    string,
    { type?: string; items?: { type?: string }; enum?: string[] }
  >;
  additionalProperties: boolean;
};

type ReadOnlySchema = {
  type: string;
  properties?: Record<string, unknown>;
  additionalProperties: boolean;
};

const GATE_ENUM = ["enforce", "warn", "off"];

/** Assert the mutating forward-compat surface (paths_hint + gate) is present
 *  on a mutating level tool's schema. */
function assertGateSurface(schema: MutatingSchema): void {
  assert.ok(schema.properties.paths_hint, "must expose `paths_hint`");
  assert.ok(schema.properties.gate, "must expose `gate`");
  assert.deepEqual(schema.properties.gate.enum, GATE_ENUM);
  assert.equal(schema.additionalProperties, false);
}

// ---------------------------------------------------------------------------
// level_open — mutating, dirty guard, gate deferred.
// ---------------------------------------------------------------------------

test("level_open is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(levelOpen.name, "unreal_open_mcp_level_open");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_level_open"));
});

test("level_open schema makes path required and exposes ignore_dirty + gate", () => {
  const schema = levelOpen.inputSchema as MutatingSchema;
  assert.equal(schema.type, "object");
  assert.deepEqual(schema.required, ["path"]);
  assert.ok(schema.properties.path, "must expose `path`");
  assert.equal(schema.properties.path.type, "string");
  // Dirty-guard bypass flag.
  assert.ok(schema.properties.ignore_dirty, "must expose `ignore_dirty`");
  assertGateSurface(schema);
});

test("level_open description documents mutation, the dirty guard, the gate deferral, and error codes", () => {
  const desc = levelOpen.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /defer/i, "must document the gate deferral");
  // Dirty guard + its bypass.
  assert.match(desc, /ignore_dirty/);
  assert.match(desc, /level_dirty/);
  // Content-path model.
  assert.match(desc, /\/game/i);
  // Shared + tool-specific error codes.
  assert.match(desc, /level_not_found/);
  assert.match(desc, /invalid_path/);
  assert.match(desc, /no_editor/);
});

// ---------------------------------------------------------------------------
// level_save — mutating, save-in-place vs save-as, gate deferred.
// ---------------------------------------------------------------------------

test("level_save is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(levelSave.name, "unreal_open_mcp_level_save");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_level_save"));
});

test("level_save schema exposes an optional path (save-as) + gate, no required args", () => {
  const schema = levelSave.inputSchema as MutatingSchema;
  assert.equal(schema.type, "object");
  // Save-in-place (no args) is valid, so path is NOT required.
  assert.equal(schema.required, undefined);
  assert.ok(schema.properties.path, "must expose `path`");
  assert.equal(schema.properties.path.type, "string");
  assertGateSurface(schema);
});

test("level_save description documents mutation, save-as vs in-place, the gate deferral, and error codes", () => {
  const desc = levelSave.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /defer/i, "must document the gate deferral");
  // Save-as vs in-place semantics + the transient-level guard.
  assert.match(desc, /save-as/i);
  assert.match(desc, /in place/i);
  // Result shape.
  assert.match(desc, /saved:/);
  assert.match(desc, /count/);
  // Error codes.
  assert.match(desc, /save_failed/);
  assert.match(desc, /invalid_path/);
  assert.match(desc, /no_editor_world/);
});

// ---------------------------------------------------------------------------
// level_list_loaded — read-only, no gate.
// ---------------------------------------------------------------------------

test("level_list_loaded is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(levelListLoaded.name, "unreal_open_mcp_level_list_loaded");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_level_list_loaded"));
});

test("level_list_loaded schema is read-only (no args, no gate, additionalProperties false)", () => {
  const schema = levelListLoaded.inputSchema as ReadOnlySchema;
  assert.equal(schema.type, "object");
  // No properties = no args.
  assert.equal(Object.keys(schema.properties ?? {}).length, 0);
  assert.equal(schema.additionalProperties, false);
});

test("level_list_loaded description documents read-only, path-first identity, and the level info shape", () => {
  const desc = levelListLoaded.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  assert.match(desc, /read-only/i);
  // Path-first identity.
  assert.match(desc, /path/);
  // The level info flags an agent branches on.
  assert.match(desc, /isCurrent/i);
  assert.match(desc, /isLoaded/i);
  assert.match(desc, /isPersistent/i);
  // Result shape.
  assert.match(desc, /levels:/);
  assert.match(desc, /no_editor_world/);
});

// ---------------------------------------------------------------------------
// level_set_current — mutating, gate deferred.
// ---------------------------------------------------------------------------

test("level_set_current is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(levelSetCurrent.name, "unreal_open_mcp_level_set_current");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_level_set_current"));
});

test("level_set_current schema makes path required and exposes gate", () => {
  const schema = levelSetCurrent.inputSchema as MutatingSchema;
  assert.equal(schema.type, "object");
  assert.deepEqual(schema.required, ["path"]);
  assert.ok(schema.properties.path, "must expose `path`");
  assert.equal(schema.properties.path.type, "string");
  assertGateSurface(schema);
});

test("level_set_current description documents mutation, disambiguation, the gate deferral, and error codes", () => {
  const desc = levelSetCurrent.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /defer/i, "must document the gate deferral");
  // Short-name vs package-path disambiguation.
  assert.match(desc, /ambiguous_name/);
  assert.match(desc, /level_not_found/);
  assert.match(desc, /no_editor_world/);
});

// ---------------------------------------------------------------------------
// level_unload_sublevel — mutating, persistent-level guard, gate deferred.
// ---------------------------------------------------------------------------

test("level_unload_sublevel is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(levelUnloadSublevel.name, "unreal_open_mcp_level_unload_sublevel");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_level_unload_sublevel"));
});

test("level_unload_sublevel schema makes path required and exposes gate", () => {
  const schema = levelUnloadSublevel.inputSchema as MutatingSchema;
  assert.equal(schema.type, "object");
  assert.deepEqual(schema.required, ["path"]);
  assert.ok(schema.properties.path, "must expose `path`");
  assert.equal(schema.properties.path.type, "string");
  assertGateSurface(schema);
});

test("level_unload_sublevel description documents mutation, the persistent-level guard, the gate deferral, and error codes", () => {
  const desc = levelUnloadSublevel.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /defer/i, "must document the gate deferral");
  // Persistent-level cannot be unloaded + the wasDirty discarded-edits note.
  assert.match(desc, /persistent/i);
  assert.match(desc, /wasDirty/);
  // Error codes.
  assert.match(desc, /persistent_level/);
  assert.match(desc, /not_loaded/);
  assert.match(desc, /level_not_found/);
});
