import test from "node:test";
import assert from "node:assert/strict";
import { editorApplicationGetState } from "./editor-application-get-state.js";
import { editorApplicationSetState } from "./editor-application-set-state.js";
import { ALL_TOOLS } from "./index.js";

// P5.1 acceptance: the editor application state pair is registered under the
// `unreal_open_mcp_` prefix, get-state advertises the PIE field set, and
// set-state advertises the action enum + the mutating gate surface.

test("editor_application_get_state is registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(
    editorApplicationGetState.name,
    "unreal_open_mcp_editor_application_get_state",
  );
  assert.ok(
    ALL_TOOLS.some(
      (t) => t.name === "unreal_open_mcp_editor_application_get_state",
    ),
  );
});

test("editor_application_get_state is a read-only, no-arg tool", () => {
  const schema = editorApplicationGetState.inputSchema as {
    type: string;
    properties: Record<string, unknown>;
    additionalProperties: boolean;
  };
  assert.equal(schema.type, "object");
  assert.equal(Object.keys(schema.properties ?? {}).length, 0);
  assert.equal(schema.additionalProperties, false);
  const desc = editorApplicationGetState.description ?? "";
  // Documents the PIE field set + the poll-for-latent-transition contract.
  assert.match(desc, /isPlaying/);
  assert.match(desc, /isPaused/);
  assert.match(desc, /isSimulating/);
  assert.match(desc, /editorMap/);
  assert.match(desc, /read-only/i);
  assert.match(desc, /editor_unavailable/);
});

test("editor_application_set_state is registered and requires action", () => {
  assert.equal(
    editorApplicationSetState.name,
    "unreal_open_mcp_editor_application_set_state",
  );
  assert.ok(
    ALL_TOOLS.some(
      (t) => t.name === "unreal_open_mcp_editor_application_set_state",
    ),
  );
  const schema = editorApplicationSetState.inputSchema as {
    type: string;
    required?: string[];
    properties: Record<string, { enum?: string[] }>;
    additionalProperties: boolean;
  };
  assert.deepEqual(schema.required, ["action"]);
  // action is the start/stop/pause/resume enum.
  assert.deepEqual(schema.properties.action.enum, [
    "start",
    "stop",
    "pause",
    "resume",
  ]);
  // gate surface present for the mutating dispatch.
  assert.ok(schema.properties.paths_hint, "must expose paths_hint");
  assert.ok(schema.properties.gate, "must expose gate");
  assert.equal(schema.additionalProperties, false);
});

test("editor_application_set_state documents pending + invalid_transition", () => {
  const desc = editorApplicationSetState.description ?? "";
  assert.match(desc, /pending/i, "must document the latent pending contract");
  assert.match(desc, /invalid_transition/, "must document the conflict error");
  assert.match(desc, /paths_hint_required/, "must document the gate hint rule");
  // No silent restart signal.
  assert.match(desc, /no silent restart/i);
});
