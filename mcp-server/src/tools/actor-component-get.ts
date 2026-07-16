import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.5 — actor component get. Read-only: reads a single component on an actor.
// Returns the component's ComponentData (name + class + a properties object
// reflected from the component's Edit/Blueprint-visible FProperties via
// UStructToJsonObject) so an agent can inspect state before modifying it. The
// properties object is the read counterpart of the component_modify write bag.
//
// Intentional deltas vs Unity's component-get:
//   - The properties object is the full UStructToJsonObject reflection of the
//     component's visible FProperties, not Unity's paginated SerializedProperty
//     field-by-field dump. Unreal's FProperty reflection covers the same ground
//     in one shot; paging/drill-down by property_path is a later-phase
//     convenience.
//   - Addressing is `actor` + `component` string refs, not Unity's
//     instance_id / component_instance_id int ids.
//
// Gate: read-only, no gate, no transaction. Route: live
// (POST /tools/unreal_open_mcp_actor_component_get).
export const actorComponentGet: Tool = {
  name: "unreal_open_mcp_actor_component_get",
  description:
    "Read a single component on an actor. Read-only (no gate, no transaction). " +
    "Returns the component's ComponentData — name, class, and a `properties` " +
    "object reflected from the component's Edit/Blueprint-visible FProperties " +
    "(the read counterpart of the actor_component_modify write bag). Use this " +
    "to inspect state before modifying it. The `component` ref resolves by " +
    "name, readable name, or class (first match by class — use the full " +
    "component name when several share a class). Error codes: " +
    "missing_parameter (actor or component absent), actor_not_found, " +
    "component_not_found (component ref did not resolve on the actor), " +
    "no_editor_world. Result: { component: ComponentData }. Prefer this over " +
    "raw invoke_method reflection — structured output + addressing parity with " +
    "the rest of the actor family.",
  inputSchema: {
    type: "object",
    required: ["actor", "component"],
    properties: {
      actor: {
        type: "string",
        description:
          "Host actor ref (label → name → path).",
      },
      component: {
        type: "string",
        description:
          "Component ref — the component's UObject name or readable name, or " +
          "a class ref (first match by class is returned).",
      },
    },
    additionalProperties: false,
  },
};
