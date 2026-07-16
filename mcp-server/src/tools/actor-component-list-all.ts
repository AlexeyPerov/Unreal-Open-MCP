import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.5 — actor component list-all. Read-only: lists every component on an
// actor. Returns a components[] array of ComponentData (name + class;
// properties omitted per entry to save tokens — use actor_component_get for
// the full property dump of one component). Mirrors Unity's component-list
// surface, scoped to the host actor (the engine-wide type catalog is a
// later-phase tool).
//
// Intentional deltas vs Unity's component-list-all:
//   - Host-bound: lists components ON an actor, not every attachable type in
//     the engine. Unity's component-list-all enumerates attachable types
//     across loaded assemblies (the discovery surface for component-add); the
//     Unreal family scopes list-all to a host actor because Unreal's
//     attachable-type catalog spans native + Blueprint classes and is better
//     served by the class-discovery tools in a later phase.
//   - Properties omitted per entry. A full property dump of every component
//     would blow the token budget on a complex actor; use actor_component_get
//     for one component's full properties.
//
// Gate: read-only, no gate, no transaction. Route: live
// (POST /tools/unreal_open_mcp_actor_component_list_all).
export const actorComponentListAll: Tool = {
  name: "unreal_open_mcp_actor_component_list_all",
  description:
    "List every component on an actor. Read-only (no gate, no transaction). " +
    "Returns a components[] array of ComponentData (name + class; properties " +
    "omitted per entry to save tokens — use actor_component_get for the full " +
    "property dump of one component). Use this to discover component names " +
    "before calling actor_component_get / actor_component_modify / " +
    "actor_component_destroy. Error codes: missing_parameter (actor absent), " +
    "actor_not_found, no_editor_world. Result: " +
    "{ components: ComponentData[], count: number }. Prefer this over raw " +
    "invoke_method GetComponents — structured output + addressing parity with " +
    "the rest of the actor family.",
  inputSchema: {
    type: "object",
    required: ["actor"],
    properties: {
      actor: {
        type: "string",
        description:
          "Host actor ref (label → name → path).",
      },
    },
    additionalProperties: false,
  },
};
