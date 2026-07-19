import test from "node:test";
import assert from "node:assert/strict";
import { assetFind } from "./asset-find.js";
import { ALL_TOOLS } from "./index.js";

// The asset_find tool definition is the catalog surface advertised via
// tools/list. P4.1 acceptance: registered under the `unreal_open_mcp_`
// prefix, exposes the four filter axes + pagination, the description tells an
// agent the empty-filter → /Game default and the invalid_class_path shape.
test("asset_find tool is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(assetFind.name, "unreal_open_mcp_asset_find");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_asset_find"));
});

test("asset_find schema exposes filter axes + pagination", () => {
  const schema = assetFind.inputSchema as {
    type: string;
    properties: Record<
      string,
      { type?: string; default?: unknown; minimum?: number; maximum?: number }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // Filter axes.
  assert.ok(schema.properties.name, "must expose `name` substring filter");
  assert.ok(schema.properties.class_path, "must expose `class_path` filter");
  assert.ok(schema.properties.path, "must expose `path` scope");
  assert.ok(schema.properties.tag_key, "must expose `tag_key`");
  assert.ok(schema.properties.tag_value, "must expose `tag_value`");
  // Recursive default.
  assert.ok(schema.properties.recursive, "must expose `recursive`");
  assert.equal(schema.properties.recursive.default, true);
  // Pagination.
  assert.ok(schema.properties.offset, "must expose `offset`");
  assert.equal(schema.properties.offset.type, "integer");
  assert.ok(schema.properties.limit, "must expose `limit`");
  assert.equal(schema.properties.limit.type, "integer");
  assert.equal(schema.properties.limit.default, 100);
  assert.equal(schema.properties.limit.maximum, 1000);
  assert.equal(schema.additionalProperties, false);
});

test("asset_find description documents the /Game default + error codes", () => {
  const desc = assetFind.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  // Empty-filter → /Game default (never the whole registry incl. /Engine).
  assert.match(desc, /\/Game/i, "must document the default /Game scope");
  assert.match(desc, /read-only/i, "must signal read-only (gate-free)");
  // Pagination shape.
  assert.match(desc, /total/);
  assert.match(desc, /offset/);
  assert.match(desc, /count/);
  // Error codes surface so an agent can branch.
  assert.match(desc, /invalid_class_path/, "must document invalid_class_path");
  assert.match(desc, /missing_parameter/, "must document missing_parameter");
  assert.match(desc, /invalid_parameter/, "must document invalid_parameter");
});
