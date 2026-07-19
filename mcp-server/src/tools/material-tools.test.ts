import test from "node:test";
import assert from "node:assert/strict";
import { materialCreate } from "./material-create.js";
import { materialModify } from "./material-modify.js";
import { materialGetData } from "./material-get-data.js";
import { ALL_TOOLS } from "./index.js";

// P4.3 — material family. The three tool definitions are the catalog surface
// advertised via tools/list. P4.3 acceptance:
//   - All three registered under the `unreal_open_mcp_` prefix.
//   - The two mutators (create / modify) expose paths_hint (required) + gate
//     (enforce/warn/off).
//   - material_get_data is read-only (no paths_hint/gate).
//   - Each description documents the structured error codes an agent branches
//     on.

const MUTATORS = [
  { tool: materialCreate, name: "material_create" },
  { tool: materialModify, name: "material_modify" },
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
    // paths_hint is REQUIRED on every P4.3 mutator (gate refuses empty hint).
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
    assert.match(desc, /mutating/i);
    assert.match(desc, /paths_hint/);
    assert.match(desc, /gate/);
    // Writable-root refuse is documented.
    assert.match(desc, /invalid_content_root/);
  });
}

test("material_create schema requires parent + destination", () => {
  const schema = materialCreate.inputSchema as {
    required: string[];
    properties: Record<string, unknown>;
  };
  assert.ok(schema.required.includes("parent"), "parent is required");
  assert.ok(
    schema.required.includes("destination"),
    "destination is required",
  );
  const desc = materialCreate.description ?? "";
  // Parent-type + collision guards are the P4.3 contract.
  assert.match(desc, /parent_not_found/);
  assert.match(desc, /not_a_material/);
  assert.match(desc, /asset_already_exists/);
  // Result shape { path, parent }.
  assert.match(desc, /\{ path, parent \}/);
});

test("material_modify schema requires path + exposes scalars/vectors/textures/save", () => {
  const schema = materialModify.inputSchema as {
    required: string[];
    properties: Record<string, { type?: string; default?: unknown }>;
  };
  assert.ok(schema.required.includes("path"), "path is required");
  assert.ok(schema.properties.scalars, "must expose scalars");
  assert.equal(schema.properties.scalars.type, "object");
  assert.ok(schema.properties.vectors, "must expose vectors");
  assert.equal(schema.properties.vectors.type, "object");
  assert.ok(schema.properties.textures, "must expose textures");
  assert.equal(schema.properties.textures.type, "object");
  assert.ok(schema.properties.save, "must expose save");
  assert.equal(schema.properties.save.default, false);
  const desc = materialModify.description ?? "";
  // Empty-modify + wrong-type + apply/failed contract.
  assert.match(desc, /nothing_to_modify/);
  assert.match(desc, /not_a_material_instance/);
  assert.match(desc, /failed/);
});

test("material_get_data is read-only — no paths_hint/gate, exposes path + paths", () => {
  assert.equal(materialGetData.name, "unreal_open_mcp_material_get_data");
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_material_get_data"),
  );
  const schema = materialGetData.inputSchema as {
    type: string;
    required?: string[];
    properties: Record<string, { type?: string }>;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // Read-only — no paths_hint/gate surface.
  assert.ok(!schema.required || !schema.required.includes("paths_hint"));
  assert.ok(!schema.properties.paths_hint);
  assert.ok(!schema.properties.gate);
  // path required; optional scoped-read `paths` projection.
  assert.ok(schema.required?.includes("path"), "path is required");
  assert.ok(schema.properties.paths, "must expose paths projection");
  assert.equal(schema.properties.paths.type, "array");
  assert.equal(schema.additionalProperties, false);

  const desc = materialGetData.description ?? "";
  assert.match(desc, /read-only/i);
  assert.match(desc, /isInstance/);
  assert.match(desc, /asset_not_found/);
  assert.match(desc, /not_a_material/);
});
