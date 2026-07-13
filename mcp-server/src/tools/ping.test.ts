import test from "node:test";
import assert from "node:assert/strict";
import { ping } from "./ping.js";
import { ALL_TOOLS } from "./index.js";

// The ping tool definition is the catalog surface — the MCP server advertises
// these fields to clients via tools/list. P1.7 acceptance: the tool is
// registered, carries the `unreal_open_mcp_` prefix, takes no inputs, and has
// a description that tells an agent when to reach for it.
test("ping tool is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(ping.name, "unreal_open_mcp_ping");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_ping"));
});

test("ping tool takes no arguments", () => {
  assert.deepEqual(ping.inputSchema, {
    type: "object",
    properties: {},
    additionalProperties: false,
  });
});

test("ping tool description explains its role and failure modes", () => {
  const desc = ping.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  // The description must mention the end-to-end role so an agent reading
  // tools/list understands what the probe covers without consulting docs.
  assert.match(desc, /bridge health/i);
  // And the bridge-down vs timeout distinction so an agent knows the failure
  // shape before calling.
  assert.match(desc, /bridge-down|bridge down|bridge-offline|offline/i);
  assert.match(desc, /timeout/i);
});
