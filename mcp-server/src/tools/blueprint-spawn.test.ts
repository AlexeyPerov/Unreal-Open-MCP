import test from "node:test";
import assert from "node:assert/strict";
import { blueprintSpawn } from "./blueprint-spawn.js";
import { ALL_TOOLS } from "./index.js";

// Blueprint spawn catalog/contract tests. The tool definition is the catalog
// surface advertised via tools/list. Acceptance (mirrors P6.6 plan):
//   - Registered under the `unreal_open_mcp_` prefix.
//   - Is a mutator: exposes paths_hint (required) + gate (enforce/warn/off).
//   - Documents the headless-safe spawn path (UWorld::SpawnActor, NOT the
//     viewport-aware editor subsystem).
//   - Documents the compile-first contract (not_compiled -> compile first) +
//     the non-Actor reject (not_actor_blueprint) + the no_editor_world path.
//   - Documents the result shape { actor, name, class, path, location } and
//     the full structured error code list.

test("blueprint_spawn is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(blueprintSpawn.name, "unreal_open_mcp_blueprint_spawn");
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_blueprint_spawn"),
  );
});

test("blueprint_spawn schema exposes path + paths_hint (required) + gate + optional location/name", () => {
  const schema = blueprintSpawn.inputSchema as {
    type: string;
    required: string[];
    properties: Record<
      string,
      {
        type?: string;
        enum?: string[];
        items?: { type?: string };
        default?: string;
        properties?: Record<string, unknown>;
        additionalProperties?: boolean;
      }
    >;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  // path + paths_hint are REQUIRED (paths_hint is required on every mutator).
  assert.ok(schema.required.includes("path"), "path is required");
  assert.ok(
    schema.required.includes("paths_hint"),
    "blueprint_spawn must list paths_hint in required",
  );
  assert.ok(
    schema.properties.paths_hint,
    "blueprint_spawn must expose paths_hint",
  );
  assert.equal(schema.properties.paths_hint.type, "array");
  assert.equal(schema.properties.paths_hint.items?.type, "string");
  // gate enum + default enforce.
  assert.ok(schema.properties.gate, "blueprint_spawn must expose gate");
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  assert.equal(schema.properties.gate.default, "enforce");
  // Optional location ({x,y,z}) + name (actor label). The nested location
  // object must also reject unknown keys (additionalProperties:false) so a
  // client cannot smuggle a bogus axis.
  assert.ok(schema.properties.location, "blueprint_spawn must expose location");
  assert.equal(schema.properties.location.type, "object");
  assert.ok(schema.properties.location.properties?.x, "location has x");
  assert.ok(schema.properties.location.properties?.y, "location has y");
  assert.ok(schema.properties.location.properties?.z, "location has z");
  assert.equal(
    schema.properties.location.additionalProperties,
    false,
    "location must reject unknown axes (additionalProperties:false)",
  );
  assert.ok(schema.properties.name, "blueprint_spawn must expose name");
  assert.equal(schema.additionalProperties, false);
});

test("blueprint_spawn description documents mutation + the headless-safe spawn path", () => {
  const desc = blueprintSpawn.description ?? "";
  assert.ok(desc.length > 0, "blueprint_spawn description present");
  assert.match(desc, /mutating/i);
  assert.match(desc, /paths_hint/);
  assert.match(desc, /gate/);
  // Headless-safe spawn — the core P6.6 invariant. The description must call
  // out UWorld::SpawnActor and the explicit refusal of the viewport-aware
  // editor subsystem so an agent never expects spawn to work through it.
  assert.match(desc, /UWorld::SpawnActor/);
  assert.match(desc, /headless-safe/i);
  assert.match(desc, /viewport-aware/i);
});

test("blueprint_spawn description documents the compile-first + non-Actor + no-world contracts", () => {
  const desc = blueprintSpawn.description ?? "";
  // Compile-first: GeneratedClass must exist; an uncompiled Blueprint reports
  // not_compiled pointing at blueprint_compile.
  assert.match(desc, /not_compiled/);
  assert.match(desc, /blueprint_compile first/i);
  // Non-Actor Blueprint reject (Blueprint Function Library / Object Blueprint).
  assert.match(desc, /not_actor_blueprint/);
  // No editor world path.
  assert.match(desc, /no_editor_world/);
  // The full Phase 6 loop (create -> edit -> compile -> spawn) so an agent
  // knows spawn is the loop-closer.
  assert.match(desc, /blueprint_create/);
  assert.match(desc, /blueprint_compile/);
});

test("blueprint_spawn description documents the result shape + the full error code list", () => {
  const desc = blueprintSpawn.description ?? "";
  // Result shape — the spawn identity an agent chains from. Pin the literal
  // result-shape token rather than bare prose words (the description mentions
  // 'actor'/'class'/'path' generically many times) so a regression that drops
  // the documented DTO fails this assertion.
  assert.match(desc, /\{ actor \(label\), name, class, path, location:\{x,y,z\} \}/);
  // The full structured error code list — every guard an agent can hit.
  assert.match(desc, /missing_parameter/);
  assert.match(desc, /blueprint_not_found/);
  assert.match(desc, /not_compiled/);
  assert.match(desc, /not_actor_blueprint/);
  assert.match(desc, /no_editor_world/);
  assert.match(desc, /spawn_failed/);
  assert.match(desc, /invalid_parameter/);
});
