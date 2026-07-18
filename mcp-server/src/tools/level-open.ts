import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.6 — level open. Mutating: opens an existing level by content path,
// replacing the current editor world (FEditorFileUtils::LoadMap via
// UEditorLoadingAndSavingUtils::LoadMap). The Unreal analog of Unity's
// scene_open (Single mode only — additive open is not supported in P2;
// Unreal additive/streaming is covered by level_set_current + a future
// add-streaming tool).
//
// Intentional deltas vs Unity's scene-open:
//   - `path` is an Unreal content path (`/Game/Maps/Arena`), not Unity's
//     `Assets/Scenes/Foo.unity`. Accepts a long package name or an object
//     path (`/Game/Maps/Arena.Arena`); both are normalised before the load.
//   - No `mode` (single/additive) arg — level_open always replaces the world.
//   - Dirty guard: the bridge refuses to replace a world with unsaved edits
//     unless `ignore_dirty` is set (mirrors Unity's SceneDirtyGuard). Unity's
//     scene-open relies on the editor's native save modal instead.
//   - No domain reload — Unreal map load does not trigger assembly reload, so
//     the Unity RestartThenSettle lifecycle metadata does not apply.
//
// Gate: `paths_hint` + `gate` are accepted on the schema for forward-compat
// but NOT enforced until P3.5 (documented P2.6 deferral). Route: live
// (POST /tools/unreal_open_mcp_level_open). Mutating.
export const levelOpen: Tool = {
  name: "unreal_open_mcp_level_open",
  description:
    "Open an existing level by content path (e.g. '/Game/Maps/Arena'), " +
    "replacing the current editor world. Identify the level by its long " +
    "package name or object path. A dirty guard refuses the open when the " +
    "current level has unsaved edits — set `ignore_dirty` to bypass it " +
    "(the discarded packages are then reported in `discardedDirtyLevels` so " +
    "an unattended caller still learns what was lost). Returns the opened " +
    "level's identity { path, name, isCurrent, dirty:false }. Mutating: " +
    "replaces the editor world; the new actor context is the opened level's " +
    "persistent level. `paths_hint` + `gate` are accepted for forward-compat " +
    "but gate enforcement is deferred (no-op in P2). Error codes: " +
    "missing_parameter (path absent), invalid_path (not a valid /Game/... " +
    "package path), level_not_found (no .umap package at the path, or " +
    "LoadMap failed), level_dirty (current world has unsaved edits; set " +
    "ignore_dirty=true to discard), no_editor (GEditor null — not running in " +
    "the editor). Prefer this over raw invoke_method LoadMap — structured " +
    "output + the dirty guard + path validation.",
  inputSchema: {
    type: "object",
    required: ["path"],
    properties: {
      path: {
        type: "string",
        description:
          "Content path of the level to open. Accepts a long package name " +
          "('/Game/Maps/Arena') or an object path ('/Game/Maps/Arena.Arena'); " +
          "both are normalised. Must resolve to an existing .umap package.",
      },
      ignore_dirty: {
        type: "boolean",
        default: false,
        description:
          "Bypass the dirty guard. When false (default), the open is refused " +
          "with level_dirty if the current level has unsaved edits. When true, " +
          "the unsaved edits are discarded (the discarded package names are " +
          "reported in `discardedDirtyLevels`).",
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
