import test from "node:test";
import assert from "node:assert/strict";
import { blueprintAddVariable } from "./blueprint-add-variable.js";
import { blueprintModifyVariable } from "./blueprint-modify-variable.js";
import { blueprintSetDefault } from "./blueprint-set-default.js";
import { ALL_TOOLS } from "./index.js";

// Blueprint variable-tool catalog/contract tests. The three tool definitions
// are the catalog surface advertised via tools/list. Acceptance:
//   - All three registered under the `unreal_open_mcp_` prefix.
//   - All three are mutators: expose paths_hint (required) + gate
//     (enforce/warn/off).
//   - add_variable: requires path + name + type; optional is_array +
//     default_value; description documents the structured error codes
//     (name_collision / invalid_type / add_failed) + the result shape.
//   - modify_variable: requires path + name; optional new_name / new_type /
//     is_array; description documents the error codes (variable_not_found /
//     missing_parameter / name_collision / invalid_type) + the validate-before-
//     mutate ordering.
//   - set_default: requires path + property + value; description documents the
//     error codes (no_generated_class / property_not_found / import_failed) +
//     the compile-first note.

test("blueprint_add_variable is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(
    blueprintAddVariable.name,
    "unreal_open_mcp_blueprint_add_variable",
  );
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_blueprint_add_variable"),
  );
});

test("blueprint_modify_variable is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(
    blueprintModifyVariable.name,
    "unreal_open_mcp_blueprint_modify_variable",
  );
  assert.ok(
    ALL_TOOLS.some(
      (t) => t.name === "unreal_open_mcp_blueprint_modify_variable",
    ),
  );
});

test("blueprint_set_default is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(
    blueprintSetDefault.name,
    "unreal_open_mcp_blueprint_set_default",
  );
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_blueprint_set_default"),
  );
});

test("blueprint_add_variable schema exposes paths_hint (required) + gate + path/name/type", () => {
  const schema = blueprintAddVariable.inputSchema as {
    type: string;
    required: string[];
    properties: Record<
      string,
      {
        type?: string;
        enum?: string[];
        items?: { type?: string };
        default?: boolean;
      }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // paths_hint is REQUIRED on every mutator (gate refuses empty hint).
  assert.ok(
    schema.required.includes("paths_hint"),
    "blueprint_add_variable must list paths_hint in required",
  );
  assert.ok(
    schema.properties.paths_hint,
    "blueprint_add_variable must expose paths_hint",
  );
  assert.equal(schema.properties.paths_hint.type, "array");
  assert.equal(schema.properties.paths_hint.items?.type, "string");
  // path + name + type are required.
  assert.ok(schema.required.includes("path"), "path is required");
  assert.ok(schema.required.includes("name"), "name is required");
  assert.ok(schema.required.includes("type"), "type is required");
  // gate enum + default enforce.
  assert.ok(
    schema.properties.gate,
    "blueprint_add_variable must expose gate",
  );
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  // is_array + default_value are optional (NOT required).
  assert.ok(
    schema.properties.is_array,
    "blueprint_add_variable must expose is_array",
  );
  assert.equal(schema.properties.is_array.default, false);
  assert.ok(
    !schema.required.includes("is_array"),
    "is_array must be optional",
  );
  assert.ok(
    schema.properties.default_value,
    "blueprint_add_variable must expose default_value",
  );
  assert.ok(
    !schema.required.includes("default_value"),
    "default_value must be optional",
  );
  assert.equal(schema.additionalProperties, false);
});

test("blueprint_add_variable description documents mutation + variable error codes", () => {
  const desc = blueprintAddVariable.description ?? "";
  assert.ok(desc.length > 0, "blueprint_add_variable description present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /paths_hint/);
  assert.match(desc, /gate/);
  // The structured-error contract — every guard an agent can hit.
  assert.match(desc, /invalid_name/);
  assert.match(desc, /name_collision/);
  assert.match(desc, /invalid_type/);
  assert.match(desc, /add_failed/);
  // Result shape { variable, type }.
  assert.match(desc, /\{ variable, type \}/);
});

test("blueprint_modify_variable schema exposes paths_hint (required) + gate + path/name", () => {
  const schema = blueprintModifyVariable.inputSchema as {
    type: string;
    required: string[];
    properties: Record<
      string,
      {
        type?: string;
        enum?: string[];
        items?: { type?: string };
        default?: boolean;
      }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // paths_hint is REQUIRED on every mutator (gate refuses empty hint).
  assert.ok(
    schema.required.includes("paths_hint"),
    "blueprint_modify_variable must list paths_hint in required",
  );
  assert.ok(
    schema.properties.paths_hint,
    "blueprint_modify_variable must expose paths_hint",
  );
  assert.equal(schema.properties.paths_hint.type, "array");
  assert.equal(schema.properties.paths_hint.items?.type, "string");
  // path + name are required.
  assert.ok(schema.required.includes("path"), "path is required");
  assert.ok(schema.required.includes("name"), "name is required");
  // gate enum + default enforce.
  assert.ok(
    schema.properties.gate,
    "blueprint_modify_variable must expose gate",
  );
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  // new_name / new_type / is_array are optional (neither is individually
  // required, but the tool requires at least one of new_name / new_type).
  assert.ok(schema.properties.new_name, "must expose new_name");
  assert.ok(schema.properties.new_type, "must expose new_type");
  assert.ok(
    !schema.required.includes("new_name"),
    "new_name must be optional",
  );
  assert.ok(
    !schema.required.includes("new_type"),
    "new_type must be optional",
  );
  assert.equal(schema.properties.is_array.default, false);
  assert.equal(schema.additionalProperties, false);
});

test("blueprint_modify_variable description documents validate-before-mutate + error codes", () => {
  const desc = blueprintModifyVariable.description ?? "";
  assert.ok(desc.length > 0, "blueprint_modify_variable description present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /paths_hint/);
  assert.match(desc, /gate/);
  // The validate-before-mutate ordering — the critical P6.3 invariant.
  assert.match(desc, /validate-before-mutate/i);
  // The structured-error contract.
  assert.match(desc, /variable_not_found/);
  assert.match(desc, /missing_parameter/);
  assert.match(desc, /invalid_name/);
  assert.match(desc, /name_collision/);
  assert.match(desc, /invalid_type/);
  // Result shape { variable }.
  assert.match(desc, /\{ variable \}/);
});

test("blueprint_set_default schema exposes paths_hint (required) + gate + path/property/value", () => {
  const schema = blueprintSetDefault.inputSchema as {
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
    "blueprint_set_default must list paths_hint in required",
  );
  assert.ok(
    schema.properties.paths_hint,
    "blueprint_set_default must expose paths_hint",
  );
  assert.equal(schema.properties.paths_hint.type, "array");
  assert.equal(schema.properties.paths_hint.items?.type, "string");
  // path + property + value are required.
  assert.ok(schema.required.includes("path"), "path is required");
  assert.ok(schema.required.includes("property"), "property is required");
  assert.ok(schema.required.includes("value"), "value is required");
  // gate enum + default enforce.
  assert.ok(
    schema.properties.gate,
    "blueprint_set_default must expose gate",
  );
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  assert.equal(schema.additionalProperties, false);
});

test("blueprint_set_default description documents compile-first + CDO error codes", () => {
  const desc = blueprintSetDefault.description ?? "";
  assert.ok(desc.length > 0, "blueprint_set_default description present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /paths_hint/);
  assert.match(desc, /gate/);
  // Compile-first note — the critical P6.3 invariant for newly added vars.
  assert.match(desc, /compile-first/i);
  assert.match(desc, /blueprint_compile/);
  // The P6.3 structured-error contract.
  assert.match(desc, /no_generated_class/);
  assert.match(desc, /no_cdo/);
  assert.match(desc, /property_not_found/);
  assert.match(desc, /import_failed/);
  // Affects new instances only — the CDO contract.
  assert.match(desc, /newly-spawned instances/i);
  // Result shape { property, value }.
  assert.match(desc, /\{ property, value \}/);
});
