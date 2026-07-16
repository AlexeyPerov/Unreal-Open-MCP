import test from "node:test";
import assert from "node:assert/strict";
import type { Tool } from "@modelcontextprotocol/sdk/types.js";
import { actorSetParent } from "./actor-set-parent.js";
import { actorDuplicate } from "./actor-duplicate.js";
import { actorDestroy } from "./actor-destroy.js";
import { actorComponentAdd } from "./actor-component-add.js";
import { actorComponentDestroy } from "./actor-component-destroy.js";
import { actorComponentGet } from "./actor-component-get.js";
import { actorComponentModify } from "./actor-component-modify.js";
import { actorComponentListAll } from "./actor-component-list-all.js";
import { ALL_TOOLS } from "./index.js";

// The P2.5 tool definitions are the catalog surface advertised via tools/list.
// These tests pin acceptance: every tool is registered under the
// `unreal_open_mcp_actor_*` prefix, exposes the documented required + optional
// args, marks mutating tools with the forward-compat paths_hint/gate surface
// (and documents the gate deferral), marks read-only tools gate-free, and
// surfaces the structured error codes an agent branches on.

const MUTATING: Tool[] = [
  actorSetParent,
  actorDuplicate,
  actorDestroy,
  actorComponentAdd,
  actorComponentDestroy,
  actorComponentModify,
];

const READONLY: Tool[] = [actorComponentGet, actorComponentListAll];

const ALL_P25: Tool[] = [...MUTATING, ...READONLY];

test("every P2.5 tool is registered in ALL_TOOLS under the unreal_open_mcp_ prefix", () => {
  for (const tool of ALL_P25) {
    assert.ok(
      tool.name.startsWith("unreal_open_mcp_"),
      `tool ${tool.name} must use the unreal_open_mcp_ prefix`,
    );
    assert.ok(
      ALL_TOOLS.some((t) => t.name === tool.name),
      `tool ${tool.name} must be in ALL_TOOLS`,
    );
  }
});

test("every P2.5 tool schema is an object with additionalProperties:false", () => {
  for (const tool of ALL_P25) {
    const schema = tool.inputSchema as {
      type: string;
      additionalProperties: boolean;
    };
    assert.equal(schema.type, "object", `${tool.name}: type must be object`);
    assert.equal(
      schema.additionalProperties,
      false,
      `${tool.name}: additionalProperties must be false`,
    );
  }
});

// ---- Tree ops --------------------------------------------------------------

test("actor_set_parent schema makes actor + parent required and exposes keepWorldTransform", () => {
  const schema = actorSetParent.inputSchema as {
    required: string[];
    properties: Record<string, { type?: string; enum?: string[]; default?: unknown }>;
  };
  assert.deepEqual(schema.required, ["actor", "parent"]);
  assert.equal(schema.properties.actor.type, "string");
  assert.equal(schema.properties.parent.type, "string");
  assert.equal(schema.properties.keepWorldTransform.type, "boolean");
  assert.equal(schema.properties.keepWorldTransform.default, true);
  // Forward-compat gate surface.
  assert.ok(schema.properties.paths_hint);
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
});

test("actor_set_parent description documents mutation, the cycle guard, and error codes", () => {
  const desc = actorSetParent.description ?? "";
  assert.match(desc, /mutating/i);
  assert.match(desc, /defer/i, "must document the gate deferral");
  assert.match(desc, /cycle/i, "must document the cycle guard");
  assert.match(desc, /would_create_cycle/);
  assert.match(desc, /actor_not_found/);
  assert.match(desc, /parent_not_found/);
  assert.match(desc, /missing_root_component/);
});

test("actor_duplicate schema makes actor required and accepts name/parent/offset", () => {
  const schema = actorDuplicate.inputSchema as {
    required: string[];
    properties: Record<string, { type?: string; enum?: string[] }>;
  };
  assert.deepEqual(schema.required, ["actor"]);
  assert.equal(schema.properties.actor.type, "string");
  assert.equal(schema.properties.name.type, "string");
  assert.equal(schema.properties.parent.type, "string");
  assert.equal(schema.properties.offset.type, "object");
  assert.ok(schema.properties.paths_hint);
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
});

test("actor_duplicate description documents mutation and the spawn-from-template clone", () => {
  const desc = actorDuplicate.description ?? "";
  assert.match(desc, /mutating/i);
  assert.match(desc, /defer/i, "must document the gate deferral");
  assert.match(desc, /template/i, "must document spawn-from-template");
  assert.match(desc, /actor_not_found/);
  assert.match(desc, /spawn_failed/);
});

test("actor_destroy schema accepts actor (single) and actors (batch) with no required field", () => {
  const schema = actorDestroy.inputSchema as {
    required?: string[];
    properties: Record<string, { type?: string; items?: { type?: string }; enum?: string[] }>;
  };
  // Both targeting fields are optional (one of the two is enforced at the
  // handler level as missing_parameter — the schema itself has no required
  // array so either shape is syntactically valid).
  assert.equal(schema.required, undefined);
  assert.equal(schema.properties.actor.type, "string");
  assert.equal(schema.properties.actors.type, "array");
  assert.equal(schema.properties.actors.items?.type, "string");
  assert.ok(schema.properties.paths_hint);
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
});

test("actor_destroy description documents single + batch and the resolve-before-mutate contract", () => {
  const desc = actorDestroy.description ?? "";
  assert.match(desc, /mutating/i);
  assert.match(desc, /defer/i, "must document the gate deferral");
  assert.match(desc, /batch/i, "must document batch support");
  assert.match(desc, /actor_not_found/);
  assert.match(desc, /destroy_failed/);
});

// ---- Component ops ---------------------------------------------------------

test("actor_component_add schema makes actor + componentClass required", () => {
  const schema = actorComponentAdd.inputSchema as {
    required: string[];
    properties: Record<string, { type?: string; enum?: string[] }>;
  };
  assert.deepEqual(schema.required, ["actor", "componentClass"]);
  assert.equal(schema.properties.actor.type, "string");
  assert.equal(schema.properties.componentClass.type, "string");
  assert.equal(schema.properties.name.type, "string");
  assert.ok(schema.properties.paths_hint);
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
});

test("actor_component_add description documents the registration sequence and error codes", () => {
  const desc = actorComponentAdd.description ?? "";
  assert.match(desc, /mutating/i);
  assert.match(desc, /defer/i, "must document the gate deferral");
  assert.match(desc, /registercomponent/i, "must document the registration sequence");
  assert.match(desc, /class_not_found/);
  assert.match(desc, /invalid_parameter/);
  assert.match(desc, /name_conflict/);
});

test("actor_component_destroy schema makes actor + component required", () => {
  const schema = actorComponentDestroy.inputSchema as {
    required: string[];
    properties: Record<string, { type?: string; enum?: string[] }>;
  };
  assert.deepEqual(schema.required, ["actor", "component"]);
  assert.equal(schema.properties.actor.type, "string");
  assert.equal(schema.properties.component.type, "string");
  assert.ok(schema.properties.paths_hint);
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
});

test("actor_component_destroy description documents the instance-component gate", () => {
  const desc = actorComponentDestroy.description ?? "";
  assert.match(desc, /mutating/i);
  assert.match(desc, /defer/i, "must document the gate deferral");
  assert.match(desc, /instance/i, "must document the instance-component gate");
  assert.match(desc, /not_instance_component/);
  assert.match(desc, /component_not_found/);
});

test("actor_component_modify schema makes actor + component + properties required", () => {
  const schema = actorComponentModify.inputSchema as {
    required: string[];
    properties: Record<string, { type?: string; enum?: string[]; additionalProperties?: unknown }>;
  };
  assert.deepEqual(schema.required, ["actor", "component", "properties"]);
  assert.equal(schema.properties.actor.type, "string");
  assert.equal(schema.properties.component.type, "string");
  assert.equal(schema.properties.properties.type, "object");
  assert.ok(schema.properties.paths_hint);
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
});

test("actor_component_modify description documents partial success and transform shortcuts", () => {
  const desc = actorComponentModify.description ?? "";
  assert.match(desc, /mutating/i);
  assert.match(desc, /defer/i, "must document the gate deferral");
  assert.match(desc, /partial/i, "must document partial success");
  assert.match(desc, /relativeLocation/);
  assert.match(desc, /relativeRotation/);
  assert.match(desc, /component_not_found/);
});

// Read-only component tools — no gate, no paths_hint.

test("actor_component_get is read-only: no gate, no paths_hint, actor + component required", () => {
  const schema = actorComponentGet.inputSchema as {
    required: string[];
    properties: Record<string, { type?: string }>;
  };
  assert.deepEqual(schema.required, ["actor", "component"]);
  assert.equal(schema.properties.actor.type, "string");
  assert.equal(schema.properties.component.type, "string");
  assert.equal(schema.properties.paths_hint, undefined, "read-only tools carry no paths_hint");
  assert.equal(schema.properties.gate, undefined, "read-only tools carry no gate");
  const desc = actorComponentGet.description ?? "";
  assert.match(desc, /read-only/i);
  assert.match(desc, /component_not_found/);
});

test("actor_component_list_all is read-only: no gate, no paths_hint, actor required", () => {
  const schema = actorComponentListAll.inputSchema as {
    required: string[];
    properties: Record<string, { type?: string }>;
  };
  assert.deepEqual(schema.required, ["actor"]);
  assert.equal(schema.properties.actor.type, "string");
  assert.equal(schema.properties.paths_hint, undefined, "read-only tools carry no paths_hint");
  assert.equal(schema.properties.gate, undefined, "read-only tools carry no gate");
  const desc = actorComponentListAll.description ?? "";
  assert.match(desc, /read-only/i);
  assert.match(desc, /actor_not_found/);
});

test("every mutating P2.5 tool documents the gate deferral; read-only tools document read-only", () => {
  for (const tool of MUTATING) {
    const desc = tool.description ?? "";
    assert.match(desc, /mutating/i, `${tool.name}: must mark itself mutating`);
    assert.match(desc, /defer/i, `${tool.name}: must document the gate deferral`);
  }
  for (const tool of READONLY) {
    const desc = tool.description ?? "";
    assert.match(desc, /read-only/i, `${tool.name}: must mark itself read-only`);
  }
});
