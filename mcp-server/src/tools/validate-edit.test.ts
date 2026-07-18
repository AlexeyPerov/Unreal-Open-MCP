import test from "node:test";
import assert from "node:assert/strict";
import { validateEdit } from "./validate-edit.js";
import { ALL_TOOLS } from "./index.js";

// The validate_edit tool definition is the catalog surface advertised via
// tools/list. P3.6 acceptance: the tool is registered under the
// `unreal_open_mcp_` prefix, makes `paths` required, exposes the
// categories / include_rules / exclude_rules filter trio, and its description
// tells an agent the rules auto-select by extension and which error codes to
// branch on.
test("validate_edit tool is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(validateEdit.name, "unreal_open_mcp_validate_edit");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_validate_edit"));
});

test("validate_edit schema makes paths required and exposes the rule-filter trio", () => {
  const schema = validateEdit.inputSchema as {
    type: string;
    required: string[];
    properties: Record<
      string,
      { type?: string; items?: { type?: string }; enum?: string[] }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  assert.deepEqual(schema.required, ["paths"]);
  assert.equal(schema.properties.paths.type, "array");
  assert.equal(schema.properties.paths.items?.type, "string");
  // Rule-filter trio (Unity parity).
  assert.ok(schema.properties.categories, "must expose `categories`");
  assert.ok(schema.properties.include_rules, "must expose `include_rules`");
  assert.ok(schema.properties.exclude_rules, "must expose `exclude_rules`");
  // Profile / paging forward-compat surface.
  assert.ok(schema.properties.profile, "must expose `profile`");
  assert.deepEqual(schema.properties.profile.enum, ["compact", "balanced", "full"]);
  assert.ok(schema.properties.page_size, "must expose `page_size`");
  assert.ok(schema.properties.cursor, "must expose `cursor`");
  // Legacy detail alias.
  assert.ok(schema.properties.detail, "must expose `detail` (legacy alias)");
  assert.deepEqual(schema.properties.detail.enum, ["summary", "normal", "verbose"]);
  assert.equal(schema.additionalProperties, false);
});

test("validate_edit description documents the rule auto-select and error codes", () => {
  const desc = validateEdit.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  // Read-only signal — meta-tools bypass GatePolicy.Execute.
  assert.match(desc, /manual verification/i);
  // Auto-select rules by extension (Unreal extension map).
  assert.match(desc, /uasset/i);
  assert.match(desc, /compile_errors/);
  assert.match(desc, /broken_soft_references/);
  // Error codes surface so an agent can branch on missing_parameter /
  // unknown_rule.
  assert.match(desc, /missing_parameter/);
  assert.match(desc, /unknown_rule/);
});
