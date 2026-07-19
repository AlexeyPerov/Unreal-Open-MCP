import test from "node:test";
import assert from "node:assert/strict";
import { assetGetData } from "./asset-get-data.js";
import { ALL_TOOLS } from "./index.js";

// The asset_get_data tool definition is the catalog surface advertised via
// tools/list. P4.1 acceptance: registered under the `unreal_open_mcp_`
// prefix, makes `path` required, exposes the optional `paths` projection,
// and the description tells an agent the AssetSummary shape + the error
// codes.
test("asset_get_data tool is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(assetGetData.name, "unreal_open_mcp_asset_get_data");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_asset_get_data"));
});

test("asset_get_data schema makes path required + exposes paths projection", () => {
  const schema = assetGetData.inputSchema as {
    type: string;
    required: string[];
    properties: Record<
      string,
      { type?: string; items?: { type?: string } }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  assert.deepEqual(schema.required, ["path"], "path is required");
  assert.ok(schema.properties.path, "must expose `path`");
  assert.equal(schema.properties.path.type, "string");
  // Optional scoped projection.
  assert.ok(schema.properties.paths, "must expose `paths` projection");
  assert.equal(schema.properties.paths.type, "array");
  assert.equal(schema.properties.paths.items?.type, "string");
  assert.equal(schema.additionalProperties, false);
});

test("asset_get_data description documents the AssetSummary shape + error codes", () => {
  const desc = assetGetData.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  // Read-only signal.
  assert.match(desc, /read-only/i);
  // The four AssetSummary fields + the tags map.
  assert.match(desc, /\bname\b/);
  assert.match(desc, /\bpath\b/);
  assert.match(desc, /\bpackage\b/);
  assert.match(desc, /\bclass\b/);
  assert.match(desc, /\btags\b/);
  // Path-or-name convention.
  assert.match(desc, /object path/i);
  assert.match(desc, /package path/i);
  // Scoped projection mention.
  assert.match(desc, /paths/);
  // Error codes surface so an agent can branch.
  assert.match(desc, /missing_parameter/);
  assert.match(desc, /asset_not_found/);
  assert.match(desc, /invalid_parameter/);
});
