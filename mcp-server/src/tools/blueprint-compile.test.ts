import test from "node:test";
import assert from "node:assert/strict";
import { blueprintCompile } from "./blueprint-compile.js";
import { ALL_TOOLS } from "./index.js";

// Blueprint compile catalog/contract tests. The tool definition is the catalog
// surface advertised via tools/list. Acceptance:
//   - Registered under the `unreal_open_mcp_` prefix.
//   - Is a mutator: exposes paths_hint (required) + gate (enforce/warn/off).
//   - Description documents the AI feedback-loop contract — the CRITICAL
//     P6.5 invariant: a failed compile is a NORMAL result (ok:true +
//     succeeded:false), NOT a transport failure. This is what lets an agent
//     treat "compile failed" as data.
//   - Description documents the result shape { succeeded, numErrors,
//     numWarnings, messages[] } + the per-message shape + the structured error
//     codes (blueprint_not_found / invalid_content_root / missing_parameter /
//     invalid_parameter) and explicitly notes a non-zero error count is NOT one
//     of those codes.

test("blueprint_compile is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(blueprintCompile.name, "unreal_open_mcp_blueprint_compile");
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_blueprint_compile"),
  );
});

test("blueprint_compile schema exposes paths_hint (required) + gate + path", () => {
  const schema = blueprintCompile.inputSchema as {
    type: string;
    required: string[];
    properties: Record<
      string,
      {
        type?: string;
        enum?: string[];
        items?: { type?: string };
        default?: string;
      }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // paths_hint is REQUIRED on every mutator (gate refuses empty hint).
  assert.ok(
    schema.required.includes("paths_hint"),
    "blueprint_compile must list paths_hint in required",
  );
  assert.ok(
    schema.properties.paths_hint,
    "blueprint_compile must expose paths_hint",
  );
  assert.equal(schema.properties.paths_hint.type, "array");
  assert.equal(schema.properties.paths_hint.items?.type, "string");
  // path is required.
  assert.ok(schema.required.includes("path"), "path is required");
  // gate enum + default enforce.
  assert.ok(schema.properties.gate, "blueprint_compile must expose gate");
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  assert.equal(schema.properties.gate.default, "enforce");
  assert.equal(schema.additionalProperties, false);
});

test("blueprint_compile description documents mutation + the AI feedback-loop contract (ok vs succeeded)", () => {
  const desc = blueprintCompile.description ?? "";
  assert.ok(desc.length > 0, "blueprint_compile description present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /paths_hint/);
  assert.match(desc, /gate/);
  // The CRITICAL P6.5 invariant — a failed compile is a normal result, not a
  // transport failure. The description must call out the ok-vs-succeeded
  // distinction so an agent never treats a failed compile as an opaque error.
  assert.match(desc, /succeeded:false/i);
  assert.match(desc, /ok:true/i);
  assert.match(desc, /NOT a transport failure/i);
});

test("blueprint_compile description documents the result + per-message shape + error codes", () => {
  const desc = blueprintCompile.description ?? "";
  // Result shape — the structured payload an agent switches on.
  assert.match(desc, /succeeded/);
  assert.match(desc, /numErrors/);
  assert.match(desc, /numWarnings/);
  assert.match(desc, /messages\[\]/);
  // Per-message shape — severity + message + best-effort node/graph.
  assert.match(desc, /severity/);
  assert.match(desc, /'error'\|'warning'\|'info'/);
  assert.match(desc, /node/);
  assert.match(desc, /graph/);
  // The P6.5 structured-error contract — every tool-level guard an agent can
  // hit (NOT the compile error count — that rides through as succeeded:false).
  assert.match(desc, /missing_parameter/);
  assert.match(desc, /blueprint_not_found/);
  assert.match(desc, /invalid_content_root/);
  assert.match(desc, /invalid_parameter/);
  // The explicit note that a non-zero error count is NOT a tool-level error —
  // pins the data-vs-failure boundary.
  assert.match(desc, /NOT one of these codes/i);
  assert.match(desc, /succeeded:false/i);
});
