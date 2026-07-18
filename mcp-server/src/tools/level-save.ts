import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.6 — level save. Mutating: saves the current level in place, or save-as
// the persistent level to a new content path. The Unreal analog of Unity's
// scene_save.
//
// Intentional deltas vs Unity's scene-save:
//   - `path` (optional) switches between save-in-place (omit) and save-as
//     (set). Save-as uses UEditorLoadingAndSavingUtils::SaveMap and writes the
//     PERSISTENT level to the new path; in-place uses SaveCurrentLevel.
//   - A transient/never-saved level with no `path` returns save_failed
//     (provide `path` to give it a location). Without this guard the in-place
//     path would raise a modal Save-As file dialog that blocks the game thread.
//   - Save-as force-overwrites an existing .umap silently; the collision is
//     detected and reported as `overwrote` (the save still proceeds).
//
// Gate: `paths_hint` + `gate` are accepted on the schema for forward-compat
// but NOT enforced until P3.5 (documented P2.6 deferral). Route: live
// (POST /tools/unreal_open_mcp_level_save). Mutating.
export const levelSave: Tool = {
  name: "unreal_open_mcp_level_save",
  description:
    "Save the current editor level. Omit `path` to save the current level in " +
    "place (the current level may be a streaming sublevel); pass `path` to " +
    "save-as the PERSISTENT level to a new content location. A " +
    "transient/never-saved level with no `path` returns save_failed — pass " +
    "`path` to give it a location. Save-as silently overwrites an existing " +
    ".umap at the target; the collision is detected and reported as " +
    "`overwrote` (the save still proceeds). Returns { saved: string[], " +
    "count: number } (the saved package paths; plus `savedPath` + `overwrote` " +
    "for a save-as). Mutating: writes the level package(s) to disk; clears " +
    "their dirty state. `paths_hint` + `gate` are accepted for forward-compat " +
    "but gate enforcement is deferred (no-op in P2). Error codes: " +
    "no_editor_world (no editor world), invalid_path (save-as path not a " +
    "valid package path, or a non-map asset already claims it), save_failed " +
    "(SaveMap/SaveCurrentLevel returned false, or the current level was never " +
    "saved and no `path` given). Prefer this over raw invoke_method SaveMap — " +
    "structured output + the transient-level guard + path validation.",
  inputSchema: {
    type: "object",
    properties: {
      path: {
        type: "string",
        description:
          "Save-as target content path (e.g. '/Game/Maps/Arena'). Omit to " +
          "save the current level in place. When set, the PERSISTENT level is " +
          "written to this path (accepts a long package name or object path; " +
          "both are normalised). Must resolve to a valid /Game/... location.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — content path(s) the mutation is scoped to, fed to " +
          "the gate as the checkpoint + validate hint. REQUIRED for mutating " +
          "tools (the gate refuses an empty hint with paths_hint_required; " +
          "there is no whole-project fallback). Set gate:\"off\" to bypass " +
          "the gate and skip the hint.",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint → mutate → " +
          "validate → delta and hard-fails on new Errors; warn commits " +
          "the mutation but surfaces new Errors as warnings; off skips " +
          "the gate entirely (paths_hint optional). Precedence: request " +
          "gate → tool default (enforce for mutators).",
      },
    },
    additionalProperties: false,
  },
};
