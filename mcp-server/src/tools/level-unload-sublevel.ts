import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.6 — level unload sublevel. Mutating: unloads (removes from the world) a
// loaded streaming sublevel by short name OR full package path
// (UEditorLevelUtils::RemoveLevelFromWorld). The persistent level cannot be
// unloaded. The Unreal analog of Unity's scene_unload.
//
// Intentional deltas vs Unity's scene-unload:
//   - `path` accepts a short name OR a full package path (the package path is
//     decisive when a short name matches multiple sublevels). The persistent
//     level is rejected with persistent_level (Unity's scene-unload can target
//     any opened scene; Unreal streaming has the persistent/sublevel split).
//   - The sublevel's unsaved state is captured BEFORE the remove and reported
//     as `wasDirty` — RemoveLevelFromWorld discards a dirty sublevel's edits
//     with no prompt under -unattended.
//
// Gate: `paths_hint` + `gate` are accepted on the schema for forward-compat
// but NOT enforced until P3.5 (documented P2.6 deferral). Route: live
// (POST /tools/unreal_open_mcp_level_unload_sublevel). Mutating.
export const levelUnloadSublevel: Tool = {
  name: "unreal_open_mcp_level_unload_sublevel",
  description:
    "Unload (remove from the world) a loaded streaming sublevel by short name " +
    "or full package path. The persistent level cannot be unloaded. The " +
    "sublevel's unsaved state is captured BEFORE the remove and reported as " +
    "`wasDirty` (RemoveLevelFromWorld discards a dirty sublevel's edits with " +
    "no prompt under -unattended). Use level_list_loaded to discover the " +
    "sublevel names. Returns { path, name, wasDirty }. Mutating: removes the " +
    "sublevel from the editor world. `paths_hint` + `gate` are accepted for " +
    "forward-compat but gate enforcement is deferred (no-op in P2). Error " +
    "codes: missing_parameter (path absent), no_editor_world (no editor " +
    "world), level_not_found (no streaming sublevel matches), ambiguous_name " +
    "(short name matches multiple sublevels), persistent_level (the name " +
    "resolves to the persistent level — cannot be unloaded), not_loaded (the " +
    "matched sublevel is not currently loaded), unload_failed " +
    "(RemoveLevelFromWorld returned false). Prefer this over raw invoke_method " +
    "RemoveLevelFromWorld — structured output + the persistent-level guard.",
  inputSchema: {
    type: "object",
    required: ["path"],
    properties: {
      path: {
        type: "string",
        description:
          "Short name (content-browser style, e.g. 'Sub') or full package " +
          "path ('/Game/Maps/Sub') of the streaming sublevel to unload. " +
          "Matched case-insensitively; the full package path is decisive when " +
          "a short name is ambiguous. The persistent level is rejected.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope (forward-compat) — content path the unload is scoped " +
          "to. Accepted but NOT enforced until the gate lands (P3.5).",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode (forward-compat) — accepted but ignored in P2 (gate " +
          "execution is a no-op until P3.5).",
      },
    },
    additionalProperties: false,
  },
};
