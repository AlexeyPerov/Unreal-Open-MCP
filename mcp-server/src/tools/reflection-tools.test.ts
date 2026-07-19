import test from "node:test";
import assert from "node:assert/strict";
import { reflectionMethodFind } from "./reflection-method-find.js";
import { reflectionMethodCall } from "./reflection-method-call.js";
import { ALL_TOOLS } from "./index.js";

// P5.4 acceptance: the reflection family is registered under the
// `unreal_open_mcp_` prefix (NOT Unity's find_members / invoke_method names),
// find requires `class` and advertises matched/returned honesty, and call
// advertises the target XOR class rule + the safety allow-list + mutating gate.

test("reflection tools are registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(reflectionMethodFind.name, "unreal_open_mcp_reflection_method_find");
  assert.equal(reflectionMethodCall.name, "unreal_open_mcp_reflection_method_call");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_reflection_method_find"));
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_reflection_method_call"));
  // Explicitly NOT the Unity names.
  assert.ok(!ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_find_members"));
  assert.ok(!ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_invoke_method"));
});

test("reflection_method_find requires class and documents matched/returned", () => {
  const schema = reflectionMethodFind.inputSchema as unknown as {
    required?: string[];
    properties: Record<string, unknown>;
    additionalProperties: boolean;
  };
  assert.deepEqual(schema.required, ["class"]);
  assert.ok(schema.properties.name, "name filter");
  assert.ok(schema.properties.limit, "limit");
  assert.equal(schema.additionalProperties, false);
  const desc = reflectionMethodFind.description ?? "";
  assert.match(desc, /read-only/i);
  assert.match(desc, /matched/);
  assert.match(desc, /returned/);
  assert.match(desc, /class_not_found/);
  assert.match(desc, /callable/);
});

test("reflection_method_call requires method + documents XOR/safety/gate", () => {
  const schema = reflectionMethodCall.inputSchema as unknown as {
    required?: string[];
    properties: Record<string, unknown>;
  };
  assert.deepEqual(schema.required, ["method"]);
  assert.ok(schema.properties.target, "target arg");
  assert.ok(schema.properties.class, "class arg");
  assert.ok(schema.properties.args, "args map");
  assert.ok(schema.properties.paths_hint, "paths_hint");
  assert.ok(schema.properties.gate, "gate");
  const desc = reflectionMethodCall.description ?? "";
  // Target XOR class.
  assert.match(desc, /ambiguous_target/, "documents the XOR conflict");
  assert.match(desc, /XOR|mutually exclusive/i);
  // Safety allow-list.
  assert.match(desc, /BlueprintCallable/);
  assert.match(desc, /CallInEditor/);
  assert.match(desc, /method_not_callable/);
  // Gate.
  assert.match(desc, /paths_hint_required/);
});
