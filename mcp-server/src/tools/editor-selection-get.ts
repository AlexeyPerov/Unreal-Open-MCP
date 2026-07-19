import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P5.2 — read the editor's actor selection. The Unreal analog of Unity's
// selection-get, adapted to Unreal actor selection (USelection).
//
// Returns the currently selected actors as P2 identity refs
// ({ name, label, class, path }) — the same shape actor_find emits — so a
// selection read chains straight into actor_modify / screenshot / inspect.
//
// Intentional deltas vs Unity's selection-get:
//   - Actor-only selection (Unreal editor actor selection); Unity's GameObject
//     / component multi-select has no direct parity here.
//   - Identity is the actor ref trio (name / label / path) + class, not Unity's
//     instance_id.
//
// Route: live (POST /tools/unreal_open_mcp_editor_selection_get). Read-only —
// no gate.
export const editorSelectionGet: Tool = {
  name: "unreal_open_mcp_editor_selection_get",
  description:
    "Read the editor's current actor selection. Read-only (gate-free). Returns " +
    "{ count, actors: [{ name, label, class, path }, ...] } — the same identity " +
    "shape as actor_find, so you can chain a selection read straight into " +
    "actor_modify / screenshot / inspect. An empty selection returns " +
    "{ count:0, actors:[] }. Error code: editor_unavailable (no editor). Prefer " +
    "this over raw invoke_method GEditor::GetSelectedActors — structured output " +
    "+ addressing parity with the actor family.",
  inputSchema: {
    type: "object",
    properties: {},
    additionalProperties: false,
  },
};
