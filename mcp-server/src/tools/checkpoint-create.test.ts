import test from "node:test";
import assert from "node:assert/strict";
import { checkpointCreate } from "./checkpoint-create.js";
import { ALL_TOOLS } from "./index.js";

// The checkpoint_create tool definition is the catalog surface advertised via
// tools/list. P3.6 acceptance: the tool is registered under the
// `unreal_open_mcp_` prefix, accepts `paths` + `categories` + `label`, and
// its description documents the session-scoped storage contract (hot reload
// wipes checkpoints; delta surfaces this honestly as unavailable /
// checkpointLostOnReload).
test("checkpoint_create tool is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(checkpointCreate.name, "unreal_open_mcp_checkpoint_create");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_checkpoint_create"));
});

test("checkpoint_create schema exposes paths + categories + label with no required fields", () => {
  const schema = checkpointCreate.inputSchema as {
    type: string;
    required?: string[];
    properties: Record<
      string,
      { type?: string; items?: { type?: string } }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // No required fields — `paths` defaults to "every registered rule over an
  // empty scope" (Unity parity). The agent opts into a real scope by passing
  // paths.
  assert.ok(!schema.required || schema.required.length === 0, "no required fields");
  assert.ok(schema.properties.paths, "must expose `paths`");
  assert.equal(schema.properties.paths.type, "array");
  assert.equal(schema.properties.paths.items?.type, "string");
  assert.ok(schema.properties.categories, "must expose `categories`");
  assert.equal(schema.properties.categories.type, "array");
  assert.ok(schema.properties.label, "must expose `label`");
  assert.equal(schema.properties.label.type, "string");
  assert.equal(schema.additionalProperties, false);
});

test("checkpoint_create description documents the session-scoped storage contract", () => {
  const desc = checkpointCreate.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  // Session-scoped storage — hot reload wipes every checkpoint.
  assert.match(desc, /session-scoped/i);
  assert.match(desc, /hot reload/i);
  // The missing-checkpoint path on a subsequent delta call is surfaced
  // honestly, not as a hard error.
  assert.match(desc, /unavailable/i);
  assert.match(desc, /checkpointLostOnReload/i);
  // Pair-with-delta signal so an agent knows the workflow.
  assert.match(desc, /unreal_open_mcp_delta/);
});
