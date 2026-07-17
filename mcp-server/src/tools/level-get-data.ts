import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// level_get_data — read-only actor roster of the current editor world (or a
// loaded streaming sublevel named by `path`), with a token-budget profile
// (compact/balanced/full) + pagination (page_size/cursor). The Unreal analog
// of Unity's scene_get_data, adapted to Unreal's actor/level model.
//
// Intentional deltas vs Unity's scene_get_data:
//   - The unit is the ACTOR, not the GameObject transform-hierarchy node.
//     Unreal actors do not expose a parent-child transform hierarchy the way
//     Unity GameObjects do (actor attachment is a separate scene-graph concern
//     covered by actor_set_parent). So the roster is a flat actor list, not a
//     nested tree; `depth` (Unity's hierarchy-depth cap) does not apply.
//   - `path` is an Unreal content path (`/Game/Maps/Arena`) or a loaded
//     level's short name, used to scope the sweep to that sublevel's actors.
//     Omit to read the whole editor world (persistent + loaded sublevels).
//   - World Partition: when the world is a PartitionedWorld, only actors in
//     LOADED cells are visible to the editor's actor iterator. The result
//     carries `worldPartition:true` + `partitionScope:"loaded-cells-only"` so
//     an agent does not mistake a sparse roster for the complete actor set.
//     There is no Unity equivalent.
//   - Profiles map onto Unity's compact/balanced/full axis but field-by-field:
//       compact  = { label, name }
//       balanced = + folder (outliner path) + class (short name)
//       full     = + transform + components (short-name array)
//     Unity's compact emits root GameObjects + childCount + components; the
//     Unreal compact is leaner (identity only) because the drill-down path is
//     actor_find / actor_component_get, not a nested walk.
//
// Gate: read-only — no gate. Route: live (POST
// /tools/unreal_open_mcp_level_get_data).
export const levelGetData: Tool = {
  name: "unreal_open_mcp_level_get_data",
  description:
    "Read an actor roster of the current editor world (or a loaded streaming " +
    "sublevel named by `path`), with a token-budget profile + pagination. " +
    "Read-only (no gate). Default (`profile: 'compact'`) returns each actor's " +
    "identity only ({ label, name }) plus the level's path/name/actorCount. " +
    "`profile: 'balanced'` adds the outliner folder path + the actor class " +
    "short name. `profile: 'full'` adds the actor transform (location/" +
    "rotation/scale) + a short-name components array so an agent can chain " +
    "into actor_modify / actor_component_get without an extra lookup. Page " +
    "large rosters with page_size/cursor (the response carries a " +
    "`pagination.next_cursor` to resume). When `path` is supplied the sweep " +
    "is scoped to that loaded (sub)level's actors (persistent + sublevel " +
    "match by full package path or short name; a short name matching multiple " +
    "loaded levels is an ambiguous_name error). World Partition levels " +
    "surface `worldPartition:true` + `partitionScope:'loaded-cells-only'` — " +
    "only actors in loaded cells are visible; unloaded cells are not in the " +
    "roster. Prefer this over level_list_loaded when you need the actors, not " +
    "the levels. Error codes: no_editor_world (no GEditor / editor world), " +
    "level_not_found (path matches no loaded level), ambiguous_name (short " +
    "name matches multiple loaded levels), invalid_cursor (cursor past the " +
    "actor stream end).",
  inputSchema: {
    type: "object",
    properties: {
      path: {
        type: "string",
        description:
          "Optional scope — full package path (e.g. '/Game/Maps/Arena') or " +
          "short name of a loaded level whose actors to read. Omit to read " +
          "the whole editor world (persistent + loaded sublevels).",
      },
      profile: {
        enum: ["compact", "balanced", "full"],
        default: "compact",
        description:
          "Token-budget output profile. 'compact' (default): actor identity " +
          "only ({ label, name }). 'balanced': + folder (outliner path) + " +
          "class (short name). 'full': + transform (location/rotation/scale) " +
          "+ components (short-name array).",
      },
      max_actors: {
        type: "integer",
        minimum: 1,
        default: 50,
        description:
          "Hard cap on the actor roster (bounds token cost). Default 50; " +
          "clamped to a max of 200. Actors past the cap are counted in " +
          "`truncated` and not emitted. Ignored when page_size is set and " +
          "the cursor is already inside the capped window.",
      },
      page_size: {
        type: "integer",
        minimum: 1,
        description:
          "Page the actor roster. When set, the response carries a " +
          "`pagination` block with a `next_cursor` to resume. Omit to " +
          "receive the whole (capped) roster in one response.",
      },
      cursor: {
        type: "string",
        description:
          "Opaque continuation token from a previous response's " +
          "`pagination.next_cursor`. Page the actor roster. A cursor past " +
          "the stream end is an invalid_cursor error.",
      },
    },
    additionalProperties: false,
  },
};
