import test from "node:test";
import assert from "node:assert/strict";
import { objectModify } from "./object-modify.js";
import { ALL_TOOLS } from "./index.js";

// The object_modify tool definition is the catalog surface advertised via
// tools/list. P2.4 acceptance: the tool is registered under the
// `unreal_open_mcp_` prefix, marks itself mutating, exposes the `object` ref +
// flat `properties` bag (plus forward-compat paths_hint/gate), and its
// description tells an agent the gate is deferred, the addressing surface
// reaches actors/components/assets, and read-only props error (not no-op).
test("object_modify tool is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(objectModify.name, "unreal_open_mcp_object_modify");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_object_modify"));
});

test("object_modify schema exposes an object ref + a properties bag", () => {
  const schema = objectModify.inputSchema as {
    type: string;
    properties: Record<
      string,
      { type?: string; items?: { type?: string }; enum?: string[] }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // The object ref — actor label/name/path, in-memory path, or asset soft path.
  assert.ok(schema.properties.object, "must expose `object`");
  assert.equal(schema.properties.object.type, "string");
  // The flat property bag (no required array — an empty {} probes resolution).
  assert.ok(schema.properties.properties, "must expose `properties`");
  assert.equal(schema.properties.properties.type, "object");
  // Forward-compat gate surface (deferred in P2).
  assert.ok(schema.properties.paths_hint, "must expose `paths_hint`");
  assert.ok(schema.properties.gate, "must expose `gate`");
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  assert.equal(schema.additionalProperties, false);
});

test("object_modify description documents mutation, the gate deferral, the addressing surface, and error codes", () => {
  const desc = objectModify.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  // Mutating signal.
  assert.match(desc, /mutating/i);
  // P2 gate deferral so an agent knows paths_hint/gate are no-ops for now.
  assert.match(desc, /defer/i, "must document the gate deferral");
  // The addressing surface reaches actors, components, and assets.
  assert.match(desc, /component/i);
  assert.match(desc, /asset/i);
  // Partial success contract: per-field errors accumulate, do not abort.
  assert.match(desc, /partial/i, "must document partial success");
  // Read-only props error explicitly (not a silent no-op).
  assert.match(desc, /read-only/i);
  // Error codes surface so an agent can branch on object_not_found etc.
  assert.match(desc, /object_not_found/);
  assert.match(desc, /missing_parameter/);
});
