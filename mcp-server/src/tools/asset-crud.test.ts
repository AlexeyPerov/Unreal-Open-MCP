import test from "node:test";
import assert from "node:assert/strict";
import { assetCreateFolder } from "./asset-create-folder.js";
import { assetCopy } from "./asset-copy.js";
import { assetMove } from "./asset-move.js";
import { assetDelete } from "./asset-delete.js";
import { assetRefresh } from "./asset-refresh.js";
import { ALL_TOOLS } from "./index.js";

// P4.2 — Content Browser CRUD family. The five tool definitions are the
// catalog surface advertised via tools/list. P4.2 acceptance:
//   - All five registered under the `unreal_open_mcp_` prefix.
//   - The four mutators (create_folder / copy / move / delete) expose
//     paths_hint (required) + gate (enforce/warn/off).
//   - asset_refresh is read-only (no paths_hint/gate) — ScanPathsSynchronous
//     only updates the in-memory registry cache.
//   - Each description documents the structured error codes an agent
//     branches on.

const MUTATORS = [
  { tool: assetCreateFolder, name: "asset_create_folder" },
  { tool: assetCopy, name: "asset_copy" },
  { tool: assetMove, name: "asset_move" },
  { tool: assetDelete, name: "asset_delete" },
];

for (const { tool, name } of MUTATORS) {
  test(`${name} tool is registered under the unreal_open_mcp_ prefix`, () => {
    assert.equal(tool.name, `unreal_open_mcp_${name}`);
    assert.ok(ALL_TOOLS.some((t) => t.name === `unreal_open_mcp_${name}`));
  });

  test(`${name} schema exposes paths_hint (required) + gate`, () => {
    const schema = tool.inputSchema as {
      type: string;
      required: string[];
      properties: Record<
        string,
        { type?: string; enum?: string[]; items?: { type?: string } }
      >;
      additionalProperties: boolean;
    };
    assert.equal(schema.type, "object");
    // paths_hint is REQUIRED on every P4.2 mutator (gate refuses empty hint).
    assert.ok(
      schema.required.includes("paths_hint"),
      `${name} must list paths_hint in required`,
    );
    assert.ok(schema.properties.paths_hint, `${name} must expose paths_hint`);
    assert.equal(schema.properties.paths_hint.type, "array");
    assert.equal(schema.properties.paths_hint.items?.type, "string");
    // gate enum + default enforce.
    assert.ok(schema.properties.gate, `${name} must expose gate`);
    assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
    assert.equal(schema.additionalProperties, false);
  });

  test(`${name} description documents mutation + error codes`, () => {
    const desc = tool.description ?? "";
    assert.ok(desc.length > 0, `${name} description present`);
    // Mutating signal + gate contract.
    assert.match(desc, /mutating/i);
    assert.match(desc, /paths_hint/);
    assert.match(desc, /gate/);
    // Writable-root refuse is documented.
    assert.match(desc, /invalid_content_root/);
  });
}

test("asset_create_folder schema requires path + documents idempotence", () => {
  const schema = assetCreateFolder.inputSchema as {
    required: string[];
    properties: Record<string, unknown>;
  };
  assert.ok(schema.required.includes("path"), "path is required");
  assert.ok(schema.properties.path);
  const desc = assetCreateFolder.description ?? "";
  // Idempotent: created:false when folder already exists.
  assert.match(desc, /idempotent/i);
  assert.match(desc, /created/);
  assert.match(desc, /missing_parameter/);
  assert.match(desc, /execution_error/);
});

test("asset_copy schema requires source + destination", () => {
  const schema = assetCopy.inputSchema as {
    required: string[];
    properties: Record<string, unknown>;
  };
  assert.ok(schema.required.includes("source"), "source is required");
  assert.ok(schema.required.includes("destination"), "destination is required");
  const desc = assetCopy.description ?? "";
  // Destination must not exist — collision is a structured error.
  assert.match(desc, /asset_already_exists/);
  assert.match(desc, /asset_not_found/);
  assert.match(desc, /invalid_path/);
});

test("asset_move schema requires source + destination + documents redirector", () => {
  const schema = assetMove.inputSchema as {
    required: string[];
    properties: Record<string, unknown>;
  };
  assert.ok(schema.required.includes("source"));
  assert.ok(schema.required.includes("destination"));
  const desc = assetMove.description ?? "";
  // A redirector may remain at the source path.
  assert.match(desc, /redirector/i);
  assert.match(desc, /asset_already_exists/);
  assert.match(desc, /asset_not_found/);
});

test("asset_delete schema exposes force + referencer guard contract", () => {
  const schema = assetDelete.inputSchema as {
    required: string[];
    properties: Record<string, unknown>;
  };
  assert.ok(schema.required.includes("path"));
  // `force` defaults false.
  assert.ok(schema.properties.force);
  const desc = assetDelete.description ?? "";
  // Referencer guard is the P4.2 contract — REFUSES with the list unless
  // force:true.
  assert.match(desc, /delete_blocked_by_referencers/);
  assert.match(desc, /force/i);
  assert.match(desc, /referenc/i);
});

test("asset_refresh is read-only — no paths_hint/gate, exposes paths + force", () => {
  assert.equal(assetRefresh.name, "unreal_open_mcp_asset_refresh");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_asset_refresh"));
  const schema = assetRefresh.inputSchema as {
    type: string;
    required?: string[];
    properties: Record<
      string,
      { type?: string; default?: unknown }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // Read-only — no required paths_hint/gate surface.
  assert.ok(!schema.required || !schema.required.includes("paths_hint"));
  assert.ok(!schema.properties.paths_hint);
  assert.ok(!schema.properties.gate);
  assert.ok(schema.properties.paths, "must expose paths");
  assert.equal(schema.properties.paths.type, "array");
  assert.ok(schema.properties.path, "must expose path (single-string)");
  assert.ok(schema.properties.force);
  assert.equal(schema.properties.force.default, false);
  assert.equal(schema.additionalProperties, false);

  const desc = assetRefresh.description ?? "";
  assert.match(desc, /read-only/i);
  // The description names the Unreal API surface (case-insensitive) so an
  // agent reading tools/list can branch on the underlying op.
  assert.match(desc, /ScanPathsSynchronous/i);
  assert.match(desc, /\/Game/);
  assert.match(desc, /invalid_parameter/);
});
