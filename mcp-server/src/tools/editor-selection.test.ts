import test from "node:test";
import assert from "node:assert/strict";
import { editorSelectionGet } from "./editor-selection-get.js";
import { editorSelectionSet } from "./editor-selection-set.js";
import { ALL_TOOLS } from "./index.js";

// P5.2 acceptance: the editor selection pair is registered under the
// `unreal_open_mcp_` prefix, get advertises the identity-ref result shape, and
// set advertises the actors/clear args + the mutating gate surface and the
// refuse-empty / resolve-before-mutate contract.

test("editor_selection_get is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(editorSelectionGet.name, "unreal_open_mcp_editor_selection_get");
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_editor_selection_get"),
  );
});

test("editor_selection_get is a read-only, no-arg tool with identity-ref output", () => {
  const schema = editorSelectionGet.inputSchema as {
    type: string;
    properties: Record<string, unknown>;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  assert.equal(Object.keys(schema.properties ?? {}).length, 0);
  assert.equal(schema.additionalProperties, false);
  const desc = editorSelectionGet.description ?? "";
  assert.match(desc, /read-only/i);
  assert.match(desc, /count/);
  // Identity-ref field set matching the actor family.
  assert.match(desc, /label/);
  assert.match(desc, /path/);
  assert.match(desc, /editor_unavailable/);
});

test("editor_selection_set is registered and exposes actors + clear", () => {
  assert.equal(editorSelectionSet.name, "unreal_open_mcp_editor_selection_set");
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_editor_selection_set"),
  );
  const schema = editorSelectionSet.inputSchema as {
    type: string;
    properties: Record<string, { type?: string }>;
    additionalProperties: boolean;
  };
  assert.ok(schema.properties.actors, "must expose actors");
  assert.equal(schema.properties.actors.type, "array");
  assert.ok(schema.properties.clear, "must expose clear");
  assert.equal(schema.properties.clear.type, "boolean");
  // gate surface present for the mutating dispatch.
  assert.ok(schema.properties.paths_hint, "must expose paths_hint");
  assert.ok(schema.properties.gate, "must expose gate");
  assert.equal(schema.additionalProperties, false);
});

test("editor_selection_set documents refuse-empty + resolve-before-mutate + gate", () => {
  const desc = editorSelectionSet.description ?? "";
  assert.match(desc, /clear/, "must document the explicit clear flag");
  assert.match(desc, /missing_parameter/, "must document the refuse-empty error");
  assert.match(desc, /never silently wiped/i, "must document no silent wipe");
  assert.match(desc, /actor_not_found/, "must document the bad-ref abort");
  assert.match(desc, /paths_hint_required/, "must document the gate hint rule");
});
