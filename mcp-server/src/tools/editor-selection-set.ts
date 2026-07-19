import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P5.2 — drive the editor's actor selection. The Unreal analog of Unity's
// selection-set, adapted to Unreal actor selection (USelection).
//
// Replace the selection by refs, or clear it with an explicit `clear:true`.
// Resolve-all-before-mutate: every ref is resolved up front (label → name →
// path) and a single bad ref aborts the whole call with actor_not_found BEFORE
// any selection change, so a half-applied selection never lands. An empty call
// WITHOUT `clear:true` is refused with missing_parameter — the selection is
// never silently wiped (copies Unreal-MCP's empty-arg refuse, so Unity's
// empty-array "wipe" semantics do not apply here). The mutation is one
// undoable transaction and commits with a single selection-changed notify.
//
// Gate: mutating. `paths_hint` is REQUIRED — pass the level package and/or the
// selected actor paths; the gate refuses an empty hint with paths_hint_required
// (no whole-project fallback). Set gate:"off" to bypass. Route: live
// (POST /tools/unreal_open_mcp_editor_selection_set).
export const editorSelectionSet: Tool = {
  name: "unreal_open_mcp_editor_selection_set",
  description:
    "Set the editor's actor selection. Provide `actors` (an array of actor refs " +
    "resolved label → name → path) to select exactly those actors, OR set " +
    "`clear`:true to deselect all. Resolve-before-mutate: a single bad ref " +
    "aborts with actor_not_found and leaves the selection unchanged (no " +
    "half-applied selection). An empty call without clear:true is refused with " +
    "missing_parameter — the selection is never silently wiped. Returns " +
    "{ cleared, count, actors:[{ name, label, class, path }] } (same identity " +
    "shape as editor_selection_get). Mutating: `paths_hint` is required (the " +
    "gate refuses an empty hint with paths_hint_required — pass the level " +
    "package and/or selected actor paths; set gate:\"off\" to bypass). Error " +
    "codes: missing_parameter (neither clear:true nor a non-empty actors), " +
    "invalid_parameter (actors not a string array), actor_not_found (a ref did " +
    "not resolve), no_editor_world, editor_unavailable. Prefer path refs when a " +
    "label is ambiguous.",
  inputSchema: {
    type: "object",
    properties: {
      actors: {
        type: "array",
        items: { type: "string" },
        description:
          "Actor refs to select, each resolved label → name → path " +
          "(case-sensitive first, then case-insensitive). Ignored when " +
          "clear:true. Prefer full paths when a label is ambiguous.",
      },
      clear: {
        type: "boolean",
        description:
          "When true, deselect all actors (ignores `actors`). Required to " +
          "empty the selection — an empty call without this flag is refused so " +
          "the selection is never silently wiped.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — the level package path and/or selected actor " +
          "paths, fed to the gate as the checkpoint + validate hint. REQUIRED " +
          "for this mutating tool (the gate refuses an empty hint with " +
          "paths_hint_required; there is no whole-project fallback). Set " +
          'gate:"off" to bypass the gate and skip the hint.',
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint → mutate → validate → " +
          "delta and hard-fails on new Errors; warn commits but surfaces new " +
          "Errors as warnings; off skips the gate entirely (paths_hint " +
          "optional). Precedence: request gate → tool default (enforce).",
      },
    },
    additionalProperties: false,
  },
};
