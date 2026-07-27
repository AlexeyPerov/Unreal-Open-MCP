import test from "node:test";
import assert from "node:assert/strict";
import { blueprintAddComponent } from "./blueprint-add-component.js";
import { blueprintRemoveComponent } from "./blueprint-remove-component.js";
import { ALL_TOOLS } from "./index.js";

// Blueprint component-tool catalog/contract tests. The two tool definitions are
// the catalog surface advertised via tools/list. Acceptance:
//   - Both registered under the `unreal_open_mcp_` prefix.
//   - Both are mutators: expose paths_hint (required) + gate (enforce/warn/off).
//   - add_component: requires path + component_class + name; optional
//     parent_component; description documents the structured error codes
//     (no_scs / invalid_component_class / abstract_component / name_collision /
//     parent_not_found / invalid_attachment) + the result shape.
//   - remove_component: requires path + name; description documents the error
//     codes (no_scs / component_not_found) + the empty success result shape.

test("blueprint_add_component is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(
    blueprintAddComponent.name,
    "unreal_open_mcp_blueprint_add_component",
  );
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_blueprint_add_component"),
  );
});

test("blueprint_remove_component is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(
    blueprintRemoveComponent.name,
    "unreal_open_mcp_blueprint_remove_component",
  );
  assert.ok(
    ALL_TOOLS.some(
      (t) => t.name === "unreal_open_mcp_blueprint_remove_component",
    ),
  );
});

test("blueprint_add_component schema exposes paths_hint (required) + gate + the SCS args", () => {
  const schema = blueprintAddComponent.inputSchema as {
    type: string;
    required: string[];
    properties: Record<
      string,
      {
        type?: string;
        enum?: string[];
        items?: { type?: string };
      }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // paths_hint is REQUIRED on every mutator (gate refuses empty hint).
  assert.ok(
    schema.required.includes("paths_hint"),
    "blueprint_add_component must list paths_hint in required",
  );
  assert.ok(
    schema.properties.paths_hint,
    "blueprint_add_component must expose paths_hint",
  );
  assert.equal(schema.properties.paths_hint.type, "array");
  assert.equal(schema.properties.paths_hint.items?.type, "string");
  // path + component_class + name are required.
  assert.ok(schema.required.includes("path"), "path is required");
  assert.ok(
    schema.required.includes("component_class"),
    "component_class is required",
  );
  assert.ok(schema.required.includes("name"), "name is required");
  // gate enum + default enforce.
  assert.ok(
    schema.properties.gate,
    "blueprint_add_component must expose gate",
  );
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  // parent_component is the optional attach parent (NOT required).
  assert.ok(
    schema.properties.parent_component,
    "blueprint_add_component must expose parent_component",
  );
  assert.ok(
    !schema.required.includes("parent_component"),
    "parent_component must be optional",
  );
  assert.equal(schema.additionalProperties, false);
});

test("blueprint_add_component description documents mutation + SCS error codes", () => {
  const desc = blueprintAddComponent.description ?? "";
  assert.ok(desc.length > 0, "blueprint_add_component description present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /paths_hint/);
  assert.match(desc, /gate/);
  // The structured-error contract — every guard an agent can hit.
  assert.match(desc, /no_scs/);
  assert.match(desc, /invalid_component_class/);
  assert.match(desc, /abstract_component/);
  assert.match(desc, /invalid_name/);
  assert.match(desc, /name_collision/);
  assert.match(desc, /parent_not_found/);
  assert.match(desc, /invalid_attachment/);
  // Result shape { component, class }.
  assert.match(desc, /\{ component, class \}/);
});

test("blueprint_remove_component schema exposes paths_hint (required) + gate + path/name", () => {
  const schema = blueprintRemoveComponent.inputSchema as {
    type: string;
    required: string[];
    properties: Record<
      string,
      { type?: string; enum?: string[]; items?: { type?: string } }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // paths_hint is REQUIRED on every mutator (gate refuses empty hint).
  assert.ok(
    schema.required.includes("paths_hint"),
    "blueprint_remove_component must list paths_hint in required",
  );
  assert.ok(
    schema.properties.paths_hint,
    "blueprint_remove_component must expose paths_hint",
  );
  assert.equal(schema.properties.paths_hint.type, "array");
  assert.equal(schema.properties.paths_hint.items?.type, "string");
  // path + name are required.
  assert.ok(schema.required.includes("path"), "path is required");
  assert.ok(schema.required.includes("name"), "name is required");
  // gate enum + default enforce.
  assert.ok(
    schema.properties.gate,
    "blueprint_remove_component must expose gate",
  );
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  assert.equal(schema.additionalProperties, false);
});

test("blueprint_remove_component description documents mutation + SCS error codes", () => {
  const desc = blueprintRemoveComponent.description ?? "";
  assert.ok(desc.length > 0, "blueprint_remove_component description present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /paths_hint/);
  assert.match(desc, /gate/);
  // The P6.2 structured-error contract.
  assert.match(desc, /no_scs/);
  assert.match(desc, /component_not_found/);
  // Children are promoted — the contract an agent relies on.
  assert.match(desc, /promot/i);
  // Result shape: empty success.
  assert.match(desc, /\{\}/);
});
