import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.5 — actor component modify. Mutating: writes reflected FProperty values on
// a resolved component via the shared ApplyProperties helper. Same flat
// `properties` bag as actor_modify / object_modify; the scene-component
// relative-transform shortcuts (relativeLocation / relativeRotation /
// relativeScale3D, alias relativeScale) are routed to the component's
// SetRelative* APIs by the helper. Per-field errors accumulate in `errors[]`
// and do NOT abort the batch (partial success is the norm).
//
// Intentional deltas vs Unity's component-modify:
//   - A single flat `properties` object replaces Unity's `fields` array of
//     {path, value, type?} patches (SerializedProperty paths). Unreal's
//     FProperty reflection + transform shortcuts cover the same ground with a
//     name → value bag in P2.
//   - Reflection writes go through FProperty + FJsonObjectConverter (Unreal),
//     not System.Reflection + SerializedObject (Unity). Enum writes are by
//     name string, not int index.
//   - Addressing is `actor` + `component` string refs (name / readable name /
//     class), not Unity's instance_id / component_instance_id.
//
// Gate: `paths_hint` + `gate` are accepted on the schema for forward-compat but
// NOT enforced until P3.5 (documented P2.5 deferral). Route: live
// (POST /tools/unreal_open_mcp_actor_component_modify). Mutating.
export const actorComponentModify: Tool = {
  name: "unreal_open_mcp_actor_component_modify",
  description:
    "Write reflected properties on a component on an actor in the current " +
    "editor level. Pass the host `actor` ref (label → name → path) and a " +
    "`component` ref (name / readable name / class — first match by class), " +
    "then a flat `properties` name → value bag. Scene-component transform " +
    "shortcuts route to the component's SetRelative* APIs: `relativeLocation` " +
    "{x,y,z}, `relativeRotation` {pitch,yaw,roll}, `relativeScale3D` (alias " +
    "`relativeScale`) {x,y,z}; every other key is written by name through " +
    "FProperty reflection (bool/int/float/string/vector/rotator/color/enum-by-" +
    "name). The actor and component are resolved BEFORE the transaction opens, " +
    "so a miss returns actor_not_found / component_not_found with nothing " +
    "mutated. Mutating: wrapped in FScopedTransaction for editor Undo; marks " +
    "the actor package dirty. Partial success is the norm — per-field errors " +
    "accumulate in `errors[]` and do NOT abort the batch (one bad field name " +
    "does not skip the rest). `paths_hint` + `gate` are accepted for " +
    "forward-compat but gate enforcement is deferred (no-op in P2). Error " +
    "codes: missing_parameter (actor/component/properties absent), " +
    "actor_not_found, component_not_found, no_editor_world. Result: " +
    "{ applied: number, actor: string, component: string, errors?: string[] }. " +
    "Prefer this over raw invoke_method SetRelative* — structured output + " +
    "addressing parity with the rest of the actor family.",
  inputSchema: {
    type: "object",
    required: ["actor", "component", "properties"],
    properties: {
      actor: {
        type: "string",
        description:
          "Host actor ref (label → name → path). Resolved before the component, " +
          "so a bad ref returns actor_not_found with nothing mutated.",
      },
      component: {
        type: "string",
        description:
          "Component ref — the component's UObject name or readable name, or " +
          "a class ref (first match by class is returned). Resolved before the " +
          "transaction opens.",
      },
      properties: {
        type: "object",
        description:
          "Flat name → value bag. Scene-component transform shortcuts " +
          "(relativeLocation / relativeRotation / relativeScale3D) route to " +
          "the component's SetRelative* APIs; all other keys are written by " +
          "name through FProperty reflection. An empty object {} is valid " +
          "(applies nothing, useful to probe resolution).",
        additionalProperties: {},
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope (forward-compat) — level/map content path the write " +
          "is scoped to. Accepted but NOT enforced until the gate lands (P3.5).",
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
