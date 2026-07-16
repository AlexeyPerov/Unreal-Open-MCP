import test from "node:test";
import assert from "node:assert/strict";
import { actorModify } from "./actor-modify.js";
import { ALL_TOOLS } from "./index.js";

// The actor_modify tool definition is the catalog surface advertised via
// tools/list. P2.4 acceptance: the tool is registered under the
// `unreal_open_mcp_` prefix, marks itself mutating, exposes the `actor` /
// `actors` targeting pair + the flat `properties` bag (plus forward-compat
// paths_hint/gate), and its description tells an agent the gate is deferred
// and partial success is the norm.
test("actor_modify tool is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(actorModify.name, "unreal_open_mcp_actor_modify");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_actor_modify"));
});

test("actor_modify schema exposes actor/actors targeting + a properties bag", () => {
  const schema = actorModify.inputSchema as {
    type: string;
    properties: Record<
      string,
      { type?: string; items?: { type?: string }; enum?: string[] }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // Two targeting shapes: single `actor` and batch `actors`.
  assert.ok(schema.properties.actor, "must expose `actor` (single)");
  assert.equal(schema.properties.actor.type, "string");
  assert.ok(schema.properties.actors, "must expose `actors` (batch)");
  assert.equal(schema.properties.actors.type, "array");
  assert.equal(schema.properties.actors.items?.type, "string");
  // The flat property bag (no required array — an empty {} probes resolution).
  assert.ok(schema.properties.properties, "must expose `properties`");
  assert.equal(schema.properties.properties.type, "object");
  // Forward-compat gate surface (deferred in P2).
  assert.ok(schema.properties.paths_hint, "must expose `paths_hint`");
  assert.ok(schema.properties.gate, "must expose `gate`");
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  assert.equal(schema.additionalProperties, false);
});

test("actor_modify description documents mutation, the gate deferral, partial success, and error codes", () => {
  const desc = actorModify.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  // Mutating signal.
  assert.match(desc, /mutating/i);
  // P2 gate deferral so an agent knows paths_hint/gate are no-ops for now.
  assert.match(desc, /defer/i, "must document the gate deferral");
  // Partial success contract: per-field errors accumulate, do not abort.
  assert.match(desc, /partial/i, "must document partial success");
  assert.match(desc, /errors/i, "must document the errors[] field");
  // Transform shortcuts live inside `properties`.
  assert.match(desc, /location/i);
  assert.match(desc, /rotation/i);
  assert.match(desc, /scale/i);
  // Error codes surface so an agent can branch on actor_not_found etc.
  assert.match(desc, /actor_not_found/);
  assert.match(desc, /missing_parameter/);
});
