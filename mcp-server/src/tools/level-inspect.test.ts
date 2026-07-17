import test from "node:test";
import assert from "node:assert/strict";
import { levelGetData } from "./level-get-data.js";
import { levelCreate } from "./level-create.js";
import { ALL_TOOLS } from "./index.js";

// The level inspect + create tool definitions are the catalog surface
// advertised via tools/list. P2.7 acceptance: both tools are registered under
// the `unreal_open_mcp_level_` prefix; the read-only get-data tool carries no
// gate and exposes profile/pagination args; the create mutator marks itself
// mutating and carries the forward-compat `paths_hint` + `gate` surface
// (deferred until P3.5); every description tells an agent the gate deferral
// and the tool's error codes. Mirrors the one-test-per-tool catalog pattern of
// level-lifecycle.test.ts, folded into a single file per the P2.7 plan.

// ---------------------------------------------------------------------------
// Shared shape helpers.
// ---------------------------------------------------------------------------

type MutatingSchema = {
  type: string;
  required?: string[];
  properties: Record<
    string,
    { type?: string; items?: { type?: string }; enum?: string[]; default?: unknown }
  >;
  additionalProperties: boolean;
};

type ReadOnlySchema = {
  type: string;
  properties?: Record<
    string,
    { type?: string; enum?: string[]; minimum?: number; default?: unknown }
  >;
  additionalProperties: boolean;
};

const GATE_ENUM = ["enforce", "warn", "off"];
const PROFILE_ENUM = ["compact", "balanced", "full"];

// ---------------------------------------------------------------------------
// level_get_data — read-only, profile + pagination, WP-aware, no gate.
// ---------------------------------------------------------------------------

test("level_get_data is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(levelGetData.name, "unreal_open_mcp_level_get_data");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_level_get_data"));
});

test("level_get_data schema is read-only and exposes profile + pagination args, no gate", () => {
  const schema = levelGetData.inputSchema as ReadOnlySchema;
  assert.equal(schema.type, "object");
  // No required args — every parameter is optional.
  assert.equal((schema as { required?: string[] }).required, undefined);
  assert.equal(schema.additionalProperties, false);

  // profile enum + default.
  assert.ok(schema.properties?.path, "must expose `path`");
  assert.equal(schema.properties.path.type, "string");
  assert.ok(schema.properties?.profile, "must expose `profile`");
  assert.deepEqual(schema.properties.profile.enum, PROFILE_ENUM);
  assert.equal(schema.properties.profile.default, "compact");

  // Pagination args.
  assert.ok(schema.properties?.page_size, "must expose `page_size`");
  assert.equal(schema.properties.page_size.type, "integer");
  assert.equal(schema.properties.page_size.minimum, 1);
  assert.ok(schema.properties?.cursor, "must expose `cursor`");
  assert.equal(schema.properties.cursor.type, "string");

  // Hard cap.
  assert.ok(schema.properties?.max_actors, "must expose `max_actors`");
  assert.equal(schema.properties.max_actors.type, "integer");
  assert.equal(schema.properties.max_actors.minimum, 1);
  assert.equal(schema.properties.max_actors.default, 50);

  // Read-only: NO gate / paths_hint surface.
  assert.equal(schema.properties?.gate, undefined);
  assert.equal(schema.properties?.paths_hint, undefined);
});

test("level_get_data description documents read-only, profile/pagination, WP scope, and error codes", () => {
  const desc = levelGetData.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  assert.match(desc, /read-only/i);
  // Profile axis.
  assert.match(desc, /compact/i);
  assert.match(desc, /balanced/i);
  assert.match(desc, /full/i);
  // Pagination.
  assert.match(desc, /page_size/);
  assert.match(desc, /next_cursor/);
  // World Partition scope flag.
  assert.match(desc, /worldPartition/);
  assert.match(desc, /loaded-cells-only/);
  // Error codes.
  assert.match(desc, /no_editor_world/);
  assert.match(desc, /level_not_found/);
  assert.match(desc, /ambiguous_name/);
  assert.match(desc, /invalid_cursor/);
});

// ---------------------------------------------------------------------------
// level_create — mutating, dirty guard, template + save-as, gate deferred.
// ---------------------------------------------------------------------------

test("level_create is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(levelCreate.name, "unreal_open_mcp_level_create");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_level_create"));
});

test("level_create schema exposes path/template/open_after_create/ignore_dirty + gate, no required args", () => {
  const schema = levelCreate.inputSchema as MutatingSchema;
  assert.equal(schema.type, "object");
  // Every arg is optional — a bare {} creates a blank transient level.
  assert.equal(schema.required, undefined);
  assert.equal(schema.additionalProperties, false);

  assert.ok(schema.properties.path, "must expose `path`");
  assert.equal(schema.properties.path.type, "string");
  assert.ok(schema.properties.template, "must expose `template`");
  assert.equal(schema.properties.template.type, "string");
  assert.equal(schema.properties.template.default, "blank");
  assert.ok(schema.properties.open_after_create, "must expose `open_after_create`");
  assert.equal(schema.properties.open_after_create.type, "boolean");
  assert.equal(schema.properties.open_after_create.default, true);
  assert.ok(schema.properties.ignore_dirty, "must expose `ignore_dirty`");
  assert.equal(schema.properties.ignore_dirty.type, "boolean");
  assert.equal(schema.properties.ignore_dirty.default, false);

  // Forward-compat mutating surface.
  assert.ok(schema.properties.paths_hint, "must expose `paths_hint`");
  assert.ok(schema.properties.gate, "must expose `gate`");
  assert.deepEqual(schema.properties.gate.enum, GATE_ENUM);
});

test("level_create description documents mutation, the dirty guard, template/save semantics, the gate deferral, and error codes", () => {
  const desc = levelCreate.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /defer/i, "must document the gate deferral");
  // Dirty guard + bypass.
  assert.match(desc, /ignore_dirty/);
  assert.match(desc, /level_dirty/);
  assert.match(desc, /discardedDirtyLevels/);
  // Template presets + asset-path seed.
  assert.match(desc, /blank/);
  assert.match(desc, /default/);
  assert.match(desc, /template/);
  // Transient vs saved.
  assert.match(desc, /transient/i);
  assert.match(desc, /saved/);
  // Result shape.
  assert.match(desc, /overwrote/);
  // Error codes.
  assert.match(desc, /invalid_path/);
  assert.match(desc, /path_already_exists/);
  assert.match(desc, /level_not_found/);
  assert.match(desc, /create_failed/);
  assert.match(desc, /no_editor/);
});
