import test from "node:test";
import assert from "node:assert/strict";
import { actorFind } from "./actor-find.js";
import { ALL_TOOLS } from "./index.js";

// The actor_find tool definition is the catalog surface advertised via
// tools/list. P2.2 acceptance: the tool is registered under the
// `unreal_open_mcp_` prefix, exposes the two modes (targeted `actor` ref +
// list filters), and its description tells an agent the miss-vs-error shape.
test("actor_find tool is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(actorFind.name, "unreal_open_mcp_actor_find");
  assert.ok(ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_actor_find"));
});

test("actor_find schema exposes targeted + list mode args", () => {
  const schema = actorFind.inputSchema as {
    type: string;
    properties: Record<string, { type: string }>;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // Targeted mode arg.
  assert.ok(schema.properties.actor, "must expose `actor` for targeted mode");
  assert.equal(schema.properties.actor.type, "string");
  // List-mode filter args.
  assert.ok(schema.properties.class, "must expose `class` filter");
  assert.ok(schema.properties.name_contains, "must expose `name_contains` filter");
  // max_results bound.
  assert.ok(schema.properties.max_results, "must expose `max_results`");
  assert.equal(schema.properties.max_results.type, "integer");
  assert.equal(schema.additionalProperties, false);
});

test("actor_find description explains both modes and the miss shape", () => {
  const desc = actorFind.description ?? "";
  assert.ok(desc.length > 0, "description must be present");
  // Targeted mode.
  assert.match(desc, /targeted/i);
  assert.match(desc, /notFound/i, "must document the targeted-miss shape");
  // List mode.
  assert.match(desc, /list mode/i);
  assert.match(desc, /truncated/i, "must document the truncation flag");
  // Read-only signal so an agent knows no gate applies.
  assert.match(desc, /read-only/i);
});
