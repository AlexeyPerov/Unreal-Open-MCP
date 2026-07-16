import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.6 — read-only list loaded levels. Enumerates the levels loaded in the
// current editor world: the persistent level first, then any streaming
// sublevels. The Unreal analog of Unity's scene_list_opened.
//
// Intentional deltas vs Unity's scene-list-opened:
//   - Unreal's world has a persistent level + streaming sublevels (Unity has
//     a flat list of opened scenes). Each entry carries path-first identity
//     ({ path, name, isCurrent, dirty }) plus isPersistent / isLoaded /
//     isVisible flags so an agent can branch on a sublevel's state.
//   - A sublevel that is not currently loaded still appears (with
//     isLoaded:false) so the agent can see it exists and re-add it via a
//     future add-streaming tool.
//   - `dirty` is the Unreal package dirty bit (the editor's save-prompt
//     trigger), not Unity's per-scene isDirty.
//
// Route: live (POST /tools/unreal_open_mcp_level_list_loaded). Read-only — no
// gate.
export const levelListLoaded: Tool = {
  name: "unreal_open_mcp_level_list_loaded",
  description:
    "List the levels loaded in the current editor world — the persistent " +
    "level first, then any streaming sublevels. Read-only (gate-free). Each " +
    "LevelInfo entry carries { path, name, isCurrent, dirty, isPersistent, " +
    "isLoaded, isVisible } so you can branch on a sublevel's state without a " +
    "second call. `path` is the long package name (path-first identity); a " +
    "sublevel that is not currently loaded still appears with isLoaded:false. " +
    "Use the returned `path`/`name` to target level_set_current or " +
    "level_unload_sublevel. Result: { levels: LevelInfo[], count: number }. " +
    "Error codes: no_editor_world (no editor world), invalid_parameter " +
    "(malformed body). Prefer this over raw invoke_method GetLevels — " +
    "structured output + load/visibility flags + dirty state.",
  inputSchema: {
    type: "object",
    additionalProperties: false,
  },
};
