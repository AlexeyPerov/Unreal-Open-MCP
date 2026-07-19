import test from "node:test";
import assert from "node:assert/strict";
import { assetImport } from "./asset-import.js";
import { ALL_TOOLS } from "./index.js";

// P4.4 — asset import. The tool definition is the catalog surface advertised
// via tools/list. P4.4 acceptance:
//   - Registered under the `unreal_open_mcp_` prefix.
//   - Mutating: exposes paths_hint (required) + gate (enforce/warn/off).
//   - Requires `file` + `destination`; exposes optional name / replace_existing
//     / save flags (replace_existing + save default false).
//   - Description documents the structured error codes an agent branches on
//     and the supported-type guidance.

test("asset_import is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(assetImport.name, "unreal_open_mcp_asset_import");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_asset_import"));
});

test("asset_import schema exposes paths_hint (required) + gate", () => {
  const schema = assetImport.inputSchema as {
    type: string;
    required: string[];
    properties: Record<
      string,
      { type?: string; enum?: string[]; items?: { type?: string }; default?: unknown }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  assert.ok(
    schema.required.includes("paths_hint"),
    "asset_import must list paths_hint in required",
  );
  assert.ok(schema.properties.paths_hint, "asset_import must expose paths_hint");
  assert.equal(schema.properties.paths_hint.type, "array");
  assert.equal(schema.properties.paths_hint.items?.type, "string");
  // gate enum + default enforce.
  assert.ok(schema.properties.gate, "asset_import must expose gate");
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  assert.equal(schema.additionalProperties, false);
});

test("asset_import schema requires file + destination", () => {
  const schema = assetImport.inputSchema as {
    required: string[];
    properties: Record<string, { type?: string; default?: unknown }>;
  };
  assert.ok(schema.required.includes("file"), "file is required");
  assert.ok(schema.required.includes("destination"), "destination is required");
  // name is optional (not in required).
  assert.ok(!schema.required.includes("name"), "name is optional");
  assert.equal(schema.properties.file?.type, "string");
  assert.equal(schema.properties.destination?.type, "string");
});

test("asset_import exposes replace_existing + save flags defaulting to false", () => {
  const schema = assetImport.inputSchema as {
    properties: Record<string, { type?: string; default?: unknown }>;
  };
  assert.equal(schema.properties.replace_existing?.type, "boolean");
  assert.equal(schema.properties.replace_existing?.default, false);
  assert.equal(schema.properties.save?.type, "boolean");
  assert.equal(schema.properties.save?.default, false);
});

test("asset_import description documents mutation, error codes, and supported types", () => {
  const desc = assetImport.description ?? "";
  assert.ok(desc.length > 0, "description present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /paths_hint/);
  assert.match(desc, /gate/);
  // Writable-root refuse + key error codes.
  assert.match(desc, /invalid_content_root/);
  assert.match(desc, /file_not_found/);
  assert.match(desc, /asset_already_exists/);
  assert.match(desc, /import_failed/);
  // Supported-type guidance (at least one common extension family named).
  assert.match(desc, /PNG|texture|FBX|WAV/i);
});
