import test from "node:test";
import assert from "node:assert/strict";
import { blueprintCreate } from "./blueprint-create.js";
import { blueprintGet } from "./blueprint-get.js";
import { ALL_TOOLS } from "./index.js";

// Blueprint family catalog/contract tests. The two tool definitions are the
// catalog surface advertised via tools/list. Acceptance:
//   - Both registered under the `unreal_open_mcp_` prefix.
//   - create is a mutator: exposes paths_hint (required) + gate
//     (enforce/warn/off) + default parent_class; description documents the
//     structured error codes.
//   - get is read-only: no paths_hint/gate surface, only `path`; description
//     documents the summary DTO shape + error codes.

test("blueprint_create is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(blueprintCreate.name, "unreal_open_mcp_blueprint_create");
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_blueprint_create"),
  );
});

test("blueprint_get is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(blueprintGet.name, "unreal_open_mcp_blueprint_get");
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_blueprint_get"),
  );
});

test("blueprint_create schema exposes paths_hint (required) + gate + parent_class", () => {
  const schema = blueprintCreate.inputSchema as {
    type: string;
    required: string[];
    properties: Record<
      string,
      {
        type?: string;
        enum?: string[];
        items?: { type?: string };
        default?: string;
      }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // paths_hint is REQUIRED on every P6.1 mutator (gate refuses empty hint).
  assert.ok(
    schema.required.includes("paths_hint"),
    "blueprint_create must list paths_hint in required",
  );
  assert.ok(
    schema.properties.paths_hint,
    "blueprint_create must expose paths_hint",
  );
  assert.equal(schema.properties.paths_hint.type, "array");
  assert.equal(schema.properties.paths_hint.items?.type, "string");
  // path is required too.
  assert.ok(schema.required.includes("path"), "path is required");
  // gate enum + default enforce.
  assert.ok(schema.properties.gate, "blueprint_create must expose gate");
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  // parent_class defaults to Actor and is optional.
  assert.ok(
    schema.properties.parent_class,
    "blueprint_create must expose parent_class",
  );
  assert.equal(
    schema.properties.parent_class.default,
    "/Script/Engine.Actor",
  );
  assert.ok(
    !schema.required.includes("parent_class"),
    "parent_class must be optional (defaults to Actor)",
  );
  assert.equal(schema.additionalProperties, false);
});

test("blueprint_create description documents mutation + error codes", () => {
  const desc = blueprintCreate.description ?? "";
  assert.ok(desc.length > 0, "blueprint_create description present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /paths_hint/);
  assert.match(desc, /gate/);
  // The P6.1 structured-error contract.
  assert.match(desc, /parent_class_not_found/);
  assert.match(desc, /parent_not_blueprintable/);
  assert.match(desc, /invalid_package_path/);
  assert.match(desc, /invalid_content_root/);
  assert.match(desc, /asset_already_exists/);
  assert.match(desc, /create_failed/);
  // Result shape { name, path, parentClass }.
  assert.match(desc, /\{ name, path, parentClass \}/);
});

test("blueprint_get is read-only — no paths_hint/gate, exposes only path", () => {
  const schema = blueprintGet.inputSchema as {
    type: string;
    required: string[];
    properties: Record<string, { type?: string }>;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // Read-only — no paths_hint/gate surface.
  assert.ok(
    !schema.required.includes("paths_hint"),
    "blueprint_get must not require paths_hint",
  );
  assert.ok(!schema.properties.paths_hint, "no paths_hint property");
  assert.ok(!schema.properties.gate, "no gate property");
  // path is the only required arg.
  assert.ok(schema.required.includes("path"), "path is required");
  assert.equal(schema.additionalProperties, false);
});

test("blueprint_get description documents read-only + summary DTO + error codes", () => {
  const desc = blueprintGet.description ?? "";
  assert.ok(desc.length > 0, "blueprint_get description present");
  assert.match(desc, /read-only/i);
  // The summary DTO surface.
  assert.match(desc, /variables/);
  assert.match(desc, /components/);
  assert.match(desc, /functions/);
  assert.match(desc, /events/);
  assert.match(desc, /enabled/);
  assert.match(desc, /interfaces/);
  assert.match(desc, /parentChain/);
  // Error codes.
  assert.match(desc, /blueprint_not_found/);
  assert.match(desc, /missing_parameter/);
});
