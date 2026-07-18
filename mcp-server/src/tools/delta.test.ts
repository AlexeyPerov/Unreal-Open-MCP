import test from "node:test";
import assert from "node:assert/strict";
import { delta } from "./delta.js";
import { ALL_TOOLS } from "./index.js";

// The delta tool definition is the catalog surface advertised via tools/list.
// P3.6 acceptance: the tool is registered under the `unreal_open_mcp_` prefix,
// makes `checkpoint_id` required, accepts an optional `paths` override, and
// its description documents the summary shape (newErrors / newWarnings /
// resolvedErrors / resolvedWarnings) and the unavailable / lost-on-reload
// recovery path.
test("delta tool is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(delta.name, "unreal_open_mcp_delta");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_delta"));
});

test("delta schema makes checkpoint_id required and accepts a paths override", () => {
  const schema = delta.inputSchema as {
    type: string;
    required: string[];
    properties: Record<
      string,
      { type?: string; items?: { type?: string } }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  assert.deepEqual(schema.required, ["checkpoint_id"]);
  assert.equal(schema.properties.checkpoint_id.type, "string");
  // Optional paths override (defaults to the checkpoint's stored paths).
  assert.ok(schema.properties.paths, "must expose `paths`");
  assert.equal(schema.properties.paths.type, "array");
  assert.equal(schema.properties.paths.items?.type, "string");
  assert.equal(schema.additionalProperties, false);
});

test("delta description documents the summary shape and the missing-checkpoint path", () => {
  const desc = delta.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  // Summary shape (Unity parity).
  assert.match(desc, /newErrors/);
  assert.match(desc, /resolvedErrors/);
  // Missing-checkpoint recovery — not a hard error.
  assert.match(desc, /unavailable/i);
  assert.match(desc, /checkpointLostOnReload/i);
  assert.match(desc, /agentNextSteps/);
  // Error codes for the malformed-request path.
  assert.match(desc, /missing_parameter/);
});
