import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.5 — actor component destroy. Mutating: destroys a component on an actor
// in the current editor level. Only INSTANCE components (the ones the editor
// added at edit time, surfaced via AddInstanceComponent) can be destroyed
// cleanly; native/archetype components are rejected explicitly so the caller
// gets a clear reason, not a half-removed component.
//
// Intentional deltas vs Unity's component-destroy:
//   - Single component per call, addressed by `component` ref (name / readable
//     name / class). Unity's component-destroy takes a `component_types` array
//     and removes the first match per type; the Unreal family takes one ref to
//     keep the resolution unambiguous (the first-match-by-class fallback is
//     still available when `component` is a class ref).
//   - The instance-component gate (not_instance_component error) has no Unity
//     analog — Unity components are all instance components. Unreal actors
//     frequently carry native/archetype components (a StaticMeshActor's mesh,
//     a Character's movement component) that cannot be cleanly destroyed; the
//     gate prevents leaving the actor in a broken half-state.
//
// Gate: `paths_hint` + `gate` are accepted on the schema for forward-compat but
// NOT enforced until P3.5 (documented P2.5 deferral). Route: live
// (POST /tools/unreal_open_mcp_actor_component_destroy). Mutating.
export const actorComponentDestroy: Tool = {
  name: "unreal_open_mcp_actor_component_destroy",
  description:
    "Destroy a component on an actor in the current editor level. The " +
    "`component` ref resolves by name, readable name, or class (first match " +
    "by class — use the full component name when several share a class). Only " +
    "INSTANCE components (the ones the editor added at edit time) can be " +
    "destroyed cleanly; native/archetype components (a StaticMeshActor's mesh, " +
    "a Character's movement component) are rejected with not_instance_component " +
    "so the actor is never left in a broken half-state. The actor and " +
    "component are resolved BEFORE the transaction opens, so a miss returns " +
    "actor_not_found / component_not_found with nothing destroyed. Mutating: " +
    "wrapped in FScopedTransaction for editor Undo; marks the actor package " +
    "dirty. `paths_hint` + `gate` are accepted for forward-compat but gate " +
    "enforcement is deferred (no-op in P2). Error codes: missing_parameter " +
    "(actor or component absent), actor_not_found, component_not_found " +
    "(component ref did not resolve on the actor), not_instance_component " +
    "(the component is native/archetype), no_editor_world. Result: " +
    "{ destroyed: true }. Prefer this over raw invoke_method DestroyComponent — " +
    "the instance-component gate + structured errors.",
  inputSchema: {
    type: "object",
    required: ["actor", "component"],
    properties: {
      actor: {
        type: "string",
        description:
          "Host actor ref (label → name → path). Resolved before the " +
          "component, so a bad ref returns actor_not_found with nothing " +
          "destroyed.",
      },
      component: {
        type: "string",
        description:
          "Component ref — the component's UObject name or readable name, or " +
          "a class ref (first match by class is returned). Use the full " +
          "component name (from actor_component_list_all) when several " +
          "components share a class.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope (forward-compat) — level/map content path the " +
          "destroy is scoped to. Accepted but NOT enforced until the gate " +
          "lands (P3.5).",
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
