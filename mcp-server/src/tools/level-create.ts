import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// level_create — create a new, empty (or template-seeded) level and make it
// the active editor world, optionally persisting it to a content path. The
// Unreal analog of Unity's scene_create, adapted to Unreal's .umap model.
//
// Intentional deltas vs Unity's scene_create:
//   - `path` is an Unreal content path (`/Game/Maps/Arena`), not Unity's
//     `Assets/Scenes/Foo.unity`. Accepts a long package name or an object
//     path; both are normalised. Omit to leave the new level transient
//     (in-memory only).
//   - `template` accepts the named presets `blank` (default) | `default`
//     OR an asset path of an existing level to seed from
//     (e.g. '/Game/Maps/Templates/Base'). In P2 both `blank` and `default`
//     produce a bare in-memory world via GEditor->NewMap (project-specific
//     default-map template seeding is out of scope); an asset-path template
//     loads that level via NewMapFromTemplate. Unity's scene_create has an
//     empty/default setup split instead.
//   - Replaces the world (no additive create in P2). Unity's scene_create has
//     a Single/Additive mode split; Unreal additive/streaming is covered by
//     level_set_current + a future add-streaming tool. `open_after_create` is
//     accepted for schema parity but always effectively true in P2 (the
//     created world is already current).
//   - Dirty guard: the bridge refuses to replace a world with unsaved edits
//     unless `ignore_dirty` is set (mirrors level_open + Unity's
//     SceneDirtyGuard). The discarded packages are reported in
//     `discardedDirtyLevels`. Unity's scene_create relies on the editor's
//     native save modal instead.
//   - No domain reload — Unreal map create does not trigger assembly reload,
//     so the Unity RestartThenSettle lifecycle metadata does not apply.
//
// Gate: `paths_hint` + `gate` are accepted on the schema for forward-compat
// but NOT enforced until P3.5 (documented P2.7 deferral). Route: live
// (POST /tools/unreal_open_mcp_level_create). Mutating.
export const levelCreate: Tool = {
  name: "unreal_open_mcp_level_create",
  description:
    "Create a new, empty (or template-seeded) level and make it the active " +
    "editor world, optionally persisting it to a content path. Mutating: " +
    "replaces the editor world; the new actor context is the new level's " +
    "persistent level. Identify a seed via `template` ('blank' default | " +
    "'default' | an existing level's asset path); omit `path` to leave the " +
    "new level transient (in-memory only). A dirty guard refuses the create " +
    "when the current level has unsaved edits — set `ignore_dirty` to bypass " +
    "it (the discarded packages are then reported in `discardedDirtyLevels`). " +
    "Returns the new level's identity { path, name, isCurrent, dirty:false, " +
    "template, saved, savedPath?, overwrote? }. `paths_hint` + `gate` are " +
    "accepted for forward-compat but gate enforcement is deferred (no-op in " +
    "P2). Error codes: invalid_parameter (malformed body), no_editor (GEditor " +
    "null — not running in the editor), invalid_path (template or save path " +
    "not a valid /Game/... package path), level_not_found (template asset " +
    "path does not resolve to a .umap), path_already_exists (save path " +
    "collides with a non-level asset), create_failed (NewMap / NewMapFromTemplate " +
    "/ SaveMap returned null/false), level_dirty (current world has unsaved " +
    "edits; set ignore_dirty=true to discard). Prefer this over raw " +
    "invoke_method GEditor->NewMap — structured output + the dirty guard + " +
    "path validation.",
  inputSchema: {
    type: "object",
    properties: {
      path: {
        type: "string",
        description:
          "Optional content path to persist the new level to (e.g. " +
          "'/Game/Maps/NewLevel'). Accepts a long package name or an object " +
          "path; both are normalised. Omit to leave the new level transient " +
          "(in-memory only — pass `path` to give it a disk location). A " +
          "collision with an existing .umap overwrites (surfaced as " +
          "`overwrote:true`); a collision with a non-level asset is rejected " +
          "with path_already_exists.",
      },
      template: {
        type: "string",
        default: "blank",
        description:
          "Seed for the new level. 'blank' (default) and 'default' both " +
          "produce a bare in-memory world via GEditor->NewMap in P2 " +
          "(project-specific default-map template seeding is out of scope). " +
          "Any other value is treated as an asset path of an existing level " +
          "to seed from (e.g. '/Game/Maps/Templates/Base') — loaded via " +
          "NewMapFromTemplate. A non-existent asset-path template is " +
          "rejected with level_not_found BEFORE the world is replaced.",
      },
      open_after_create: {
        type: "boolean",
        default: true,
        description:
          "When true (default), the new level is the active editor world " +
          "after the create. Accepted for schema parity with Unity's " +
          "scene_create; always effectively true in P2 (the created world " +
          "is already current — there is no additive create path).",
      },
      ignore_dirty: {
        type: "boolean",
        default: false,
        description:
          "Bypass the dirty guard. When false (default), the create is " +
          "refused with level_dirty if the current level has unsaved edits. " +
          "When true, the unsaved edits are discarded (the discarded package " +
          "names are reported in `discardedDirtyLevels`).",
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
