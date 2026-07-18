import test from "node:test";
import assert from "node:assert/strict";
import { applyFix } from "./apply-fix.js";
import { ALL_TOOLS } from "./index.js";

// The apply_fix tool definition is the catalog surface advertised via
// tools/list. P3.7 acceptance: the tool is registered under the
// `unreal_open_mcp_` prefix, makes `issue_id` required, exposes the
// dry_run / gate / paths_hint trio, and its description tells an agent which
// fixes are implemented and how the rollback semantics work.
test("apply_fix tool is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(applyFix.name, "unreal_open_mcp_apply_fix");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_apply_fix"));
});

test("apply_fix schema makes issue_id required and exposes the dry_run / gate trio", () => {
  const schema = applyFix.inputSchema as {
    type: string;
    required: string[];
    properties: Record<
      string,
      { type?: string; items?: { type?: string }; enum?: string[]; default?: unknown }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  assert.deepEqual(schema.required, ["issue_id"]);
  assert.equal(schema.properties.issue_id.type, "string");
  assert.equal(schema.properties.fix_id.type, "string");
  // dry_run defaults to true (preview before mutate).
  assert.equal(schema.properties.dry_run.type, "boolean");
  assert.equal(schema.properties.dry_run.default, true);
  // Gate modes match the bridge's three tokens.
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  assert.equal(schema.properties.gate.default, "enforce");
  // paths_hint is auto-derived from issue_id when omitted.
  assert.ok(schema.properties.paths_hint, "must expose `paths_hint`");
  assert.equal(schema.properties.paths_hint.type, "array");
  assert.equal(schema.additionalProperties, false);
});

test("apply_fix description documents the implemented fixes and rollback semantics", () => {
  const desc = applyFix.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  // v1 implemented fix surfaced so an agent knows what is available.
  assert.match(desc, /clear_broken_soft_reference/);
  // Safe signal — only Safe providers are auto-suggested by the gate.
  assert.match(desc, /safe/i);
  // Rollback semantics surface so an agent understands the trust contract.
  assert.match(desc, /rollback/i);
  assert.match(desc, /enforce/i);
  // gate:"off" rollback-disabled warning — operator opt-in for no protection.
  assert.match(desc, /rollbackDisabled/);
  // dry_run default behavior.
  assert.match(desc, /dry_run/);
});
