import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.6 — level set current. Mutating: sets the current editing level (the one
// new actors are added to) by short name OR full package path
// (UEditorLevelUtils::MakeLevelCurrent). The Unreal analog of Unity's
// scene_set_active.
//
// Intentional deltas vs Unity's scene-set-active:
//   - `path` accepts a short name (content-browser style, e.g. 'Arena') OR a
//     full package path ('/Game/Maps/Arena'). The package path is unambiguous;
//     a short name matching multiple loaded levels returns ambiguous_name
//     (pass the package path to disambiguate).
//   - The current level is editor context, not a separate scene — switching it
//     does not reload anything, it just changes where new actors spawn. It is
//     idempotent (already-current is a no-op success).
//
// Gate: `paths_hint` + `gate` are accepted on the schema for forward-compat
// but NOT enforced until P3.5 (documented P2.6 deferral). Route: live
// (POST /tools/unreal_open_mcp_level_set_current). Mutating.
export const levelSetCurrent: Tool = {
  name: "unreal_open_mcp_level_set_current",
  description:
    "Set the current editing level (the level new actors are added to) by " +
    "short name or full package path. Use level_list_loaded to discover the " +
    "available names. A short name that matches multiple loaded levels is " +
    "rejected with ambiguous_name — pass the full package path to " +
    "disambiguate. Switching the current level is editor context only (no " +
    "reload); it is idempotent (already-current is a no-op success). Returns " +
    "the now-current level's identity { path, name, isCurrent:true, dirty }. " +
    "Mutating: changes the editor's actor-editing context. `paths_hint` + " +
    "`gate` are accepted for forward-compat but gate enforcement is deferred " +
    "(no-op in P2). Error codes: missing_parameter (path absent), " +
    "no_editor_world (no editor world), level_not_found (no loaded level " +
    "matches), ambiguous_name (short name matches multiple levels), " +
    "set_current_failed (MakeLevelCurrent did not take effect — the level may " +
    "be locked). Prefer this over raw invoke_method MakeLevelCurrent — " +
    "structured output + disambiguation.",
  inputSchema: {
    type: "object",
    required: ["path"],
    properties: {
      path: {
        type: "string",
        description:
          "Short name (content-browser style, e.g. 'Arena') or full package " +
          "path ('/Game/Maps/Arena') of the loaded level to make current. " +
          "Matched case-insensitively; the full package path is decisive when " +
          "a short name is ambiguous.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope (forward-compat) — content path the switch is scoped " +
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
