import test from "node:test";
import assert from "node:assert/strict";
import { blueprintAddFunction } from "./blueprint-add-function.js";
import { blueprintAddEvent } from "./blueprint-add-event.js";
import { ALL_TOOLS } from "./index.js";

// Blueprint function/event stub-tool catalog/contract tests. The two tool
// definitions are the catalog surface advertised via tools/list. Acceptance:
//   - Both registered under the `unreal_open_mcp_` prefix.
//   - Both are mutators: expose paths_hint (required) + gate (enforce/warn/off).
//   - add_function: requires path + name; description documents the structured
//     error codes (name_collision / create_graph_failed) + the result shape and
//     the stub-only scope.
//   - add_event: requires path + name; description documents the error codes
//     (not_an_event / event_already_exists / no_event_graph /
//     create_node_failed) + the ghost-enable contract + the stub-only scope.

test("blueprint_add_function is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(
    blueprintAddFunction.name,
    "unreal_open_mcp_blueprint_add_function",
  );
  assert.ok(
    ALL_TOOLS.some(
      (t) => t.name === "unreal_open_mcp_blueprint_add_function",
    ),
  );
});

test("blueprint_add_event is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(blueprintAddEvent.name, "unreal_open_mcp_blueprint_add_event");
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_blueprint_add_event"),
  );
});

test("blueprint_add_function schema exposes paths_hint (required) + gate + path/name", () => {
  const schema = blueprintAddFunction.inputSchema as {
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
    "blueprint_add_function must list paths_hint in required",
  );
  assert.ok(
    schema.properties.paths_hint,
    "blueprint_add_function must expose paths_hint",
  );
  assert.equal(schema.properties.paths_hint.type, "array");
  assert.equal(schema.properties.paths_hint.items?.type, "string");
  // path + name are required.
  assert.ok(schema.required.includes("path"), "path is required");
  assert.ok(schema.required.includes("name"), "name is required");
  // gate enum + default enforce.
  assert.ok(
    schema.properties.gate,
    "blueprint_add_function must expose gate",
  );
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  assert.equal(schema.additionalProperties, false);
});

test("blueprint_add_function description documents mutation + stub scope + error codes", () => {
  const desc = blueprintAddFunction.description ?? "";
  assert.ok(desc.length > 0, "blueprint_add_function description present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /paths_hint/);
  assert.match(desc, /gate/);
  // Stub-only scope — the critical P6.4 invariant (no node-wiring scope creep).
  assert.match(desc, /stub-only/i);
  assert.match(desc, /out of scope/i);
  // The P6.4 structured-error contract — every guard an agent can hit.
  assert.match(desc, /name_collision/);
  assert.match(desc, /create_graph_failed/);
  // Result shape { function }.
  assert.match(desc, /\{ function \}/);
});

test("blueprint_add_event schema exposes paths_hint (required) + gate + path/name", () => {
  const schema = blueprintAddEvent.inputSchema as {
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
    "blueprint_add_event must list paths_hint in required",
  );
  assert.ok(
    schema.properties.paths_hint,
    "blueprint_add_event must expose paths_hint",
  );
  assert.equal(schema.properties.paths_hint.type, "array");
  assert.equal(schema.properties.paths_hint.items?.type, "string");
  // path + name are required.
  assert.ok(schema.required.includes("path"), "path is required");
  assert.ok(schema.required.includes("name"), "name is required");
  // gate enum + default enforce.
  assert.ok(
    schema.properties.gate,
    "blueprint_add_event must expose gate",
  );
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  assert.equal(schema.additionalProperties, false);
});

test("blueprint_add_event description documents mutation + stub scope + ghost-enable + error codes", () => {
  const desc = blueprintAddEvent.description ?? "";
  assert.ok(desc.length > 0, "blueprint_add_event description present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /paths_hint/);
  assert.match(desc, /gate/);
  // Stub-only scope — the critical P6.4 invariant.
  assert.match(desc, /stub-only/i);
  assert.match(desc, /out of scope/i);
  // The ghost-enable contract — the critical P6.4 invariant (a disabled ghost
  // is ENABLED, never a false no-op).
  assert.match(desc, /ghost/i);
  // The P6.4 structured-error contract — every guard an agent can hit.
  assert.match(desc, /not_an_event/);
  assert.match(desc, /event_already_exists/);
  assert.match(desc, /no_event_graph/);
  assert.match(desc, /create_node_failed/);
  // Result shape { event, enabled:true }.
  assert.match(desc, /\{ event, enabled:true \}/);
});
