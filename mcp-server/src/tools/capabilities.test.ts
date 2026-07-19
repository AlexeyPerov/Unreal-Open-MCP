import test from "node:test";
import assert from "node:assert/strict";
import { capabilities } from "./capabilities.js";
import { ALL_TOOLS } from "./index.js";

// The capabilities tool definition is the catalog surface advertised via
// tools/list. P3.8 acceptance: the tool is registered under the
// `unreal_open_mcp_` prefix, exposes the kind / include_planned filter pair,
// and its description tells an agent the route is local and lists the v1
// implemented rules + fixes.
test("capabilities tool is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(capabilities.name, "unreal_open_mcp_capabilities");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_capabilities"));
});

test("capabilities schema exposes the kind / include_planned filter pair", () => {
  const schema = capabilities.inputSchema as {
    type: string;
    properties: Record<
      string,
      { enum?: string[]; type?: string; default?: unknown }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  assert.ok(schema.properties.kind, "must expose `kind`");
  assert.deepEqual(schema.properties.kind.enum, ["tools", "rules", "fixes"]);
  assert.ok(schema.properties.include_planned, "must expose `include_planned`");
  assert.equal(schema.properties.include_planned.type, "boolean");
  assert.equal(schema.properties.include_planned.default, true);
  assert.equal(schema.additionalProperties, false);
});

test("capabilities description documents the local route + v1 implemented surface", () => {
  const desc = capabilities.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  // Route signal — capabilities is local-only, must never depend on the bridge.
  assert.match(desc, /local/i);
  // v1 implemented rules surfaced so an agent knows what is available.
  assert.match(desc, /broken_soft_references/);
  assert.match(desc, /missing_blueprint_parents/);
  assert.match(desc, /compile_errors/);
  // v1 implemented fix surfaced so an agent knows the apply_fix surface.
  assert.match(desc, /clear_broken_soft_reference/);
  // Safe signal — only Safe providers are auto-suggested by the gate.
  assert.match(desc, /safe/i);
});
