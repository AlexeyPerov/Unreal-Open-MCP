import test from "node:test";
import assert from "node:assert/strict";
import { actorCreate } from "./actor-create.js";
import { ALL_TOOLS } from "./index.js";

// The actor_create tool definition is the catalog surface advertised via
// tools/list. P2.3 acceptance: the tool is registered under the
// `unreal_open_mcp_` prefix, marks itself mutating, exposes the required
// `classPath` + optional transform/parent args (plus forward-compat
// paths_hint/gate), and its description tells an agent the gate is deferred.
test("actor_create tool is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(actorCreate.name, "unreal_open_mcp_actor_create");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_actor_create"));
});

test("actor_create schema makes classPath required and accepts transform/parent args", () => {
  const schema = actorCreate.inputSchema as {
    type: string;
    required: string[];
    properties: Record<string, { type?: string; enum?: string[] }>;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // classPath is the one required arg.
  assert.deepEqual(schema.required, ["classPath"]);
  assert.ok(schema.properties.classPath, "must expose `classPath`");
  assert.equal(schema.properties.classPath.type, "string");
  // Optional transform + parent args.
  assert.ok(schema.properties.name, "must expose `name` (label)");
  assert.ok(schema.properties.location, "must expose `location`");
  assert.ok(schema.properties.rotation, "must expose `rotation`");
  assert.ok(schema.properties.parent, "must expose `parent`");
  // Forward-compat gate surface (deferred in P2).
  assert.ok(schema.properties.paths_hint, "must expose `paths_hint`");
  assert.ok(schema.properties.gate, "must expose `gate`");
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  assert.equal(schema.additionalProperties, false);
});

test("actor_create description documents mutation, the gate deferral, and error codes", () => {
  const desc = actorCreate.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  // Mutating signal.
  assert.match(desc, /mutating/i);
  // P2 gate deferral so an agent knows paths_hint/gate are no-ops for now.
  assert.match(desc, /defer/i, "must document the gate deferral");
  // Class-path model (native + Blueprint) replaces Unity's primitive_type.
  assert.match(desc, /classpath/i);
  assert.match(desc, /blueprint/i, "must document Blueprint asset paths");
  // Error codes surface so an agent can branch on class_not_found etc.
  assert.match(desc, /class_not_found/);
  assert.match(desc, /parent_not_found/);
});
