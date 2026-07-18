import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.5 — actor component add. Mutating: adds a component to an actor in the
// current editor level. Resolves the component class (same ResolveClass surface
// as actor_create), validates it is a non-abstract UActorComponent, creates it
// via NewObject, and runs the registration sequence (AddInstanceComponent →
// scene attach/root-set → OnComponentCreated → RegisterComponent) so the new
// component is live in the editor.
//
// Intentional deltas vs Unity's component-add:
//   - Single component per call (not an array). Unity's component-add takes a
//     `component_types` array because Unity components are cheap and frequently
//     added in groups; Unreal's NewObject + registration sequence is heavier
//     and a single class per call keeps the failure surface clean (a bad class
//     returns class_not_found with nothing created).
//   - `componentClass` is a class ref string (native soft path, Blueprint
//     generated-class path, or short type name) resolved via ResolveClass — not
//     Unity's full-name / class-name reflection over managed assemblies.
//   - The component is registered through Unreal's component lifecycle
//     (OnComponentCreated + RegisterComponent), not Unity's AddComponent. A
//     USceneComponent auto-attaches to the actor's root (or becomes the root
//     when the actor has none).
//   - A user-supplied `name` that collides with an existing component returns
//     name_conflict (a StaticAllocateObject fatal assert inside NewObject
//     would otherwise crash the editor).
//
// Gate: `paths_hint` + `gate` are accepted on the schema for forward-compat but
// NOT enforced until P3.5 (documented P2.5 deferral). Route: live
// (POST /tools/unreal_open_mcp_actor_component_add). Mutating.
export const actorComponentAdd: Tool = {
  name: "unreal_open_mcp_actor_component_add",
  description:
    "Add a component to an actor in the current editor level. Resolves the " +
    "component class, validates it is a non-abstract UActorComponent, creates " +
    "it via NewObject, and registers it (AddInstanceComponent → scene " +
    "attach/root-set → OnComponentCreated → RegisterComponent) so it is live " +
    "in the editor. A USceneComponent auto-attaches to the actor's root " +
    "(or becomes the root when the actor has none). The actor is resolved " +
    "BEFORE the transaction opens, so a bad ref returns actor_not_found with " +
    "nothing added. Returns the new component's ComponentData (name + class + " +
    "properties) so you can chain component_modify without a second call. " +
    "Mutating: wrapped in FScopedTransaction for editor Undo; marks the actor " +
    "package dirty. `paths_hint` + `gate` are accepted for forward-compat but " +
    "gate enforcement is deferred (no-op in P2). Error codes: missing_parameter " +
    "(actor or componentClass absent), actor_not_found, class_not_found " +
    "(componentClass did not resolve), invalid_parameter (resolved class is " +
    "not a UActorComponent or is abstract), name_conflict (a component with " +
    "the requested name already exists), create_failed (NewObject returned " +
    "null), no_editor_world. Result: { component: ComponentData }. Prefer " +
    "this over raw invoke_method NewObject — structured output + the full " +
    "registration sequence + addressing parity with the rest of the actor " +
    "family.",
  inputSchema: {
    type: "object",
    required: ["actor", "componentClass"],
    properties: {
      actor: {
        type: "string",
        description:
          "Host actor ref (label → name → path). Resolved before the class is " +
          "created, so a bad ref returns actor_not_found with nothing added.",
      },
      componentClass: {
        type: "string",
        description:
          "Component class ref to instantiate. Accepts a soft class path " +
          "('/Script/Engine.StaticMeshComponent'), a Blueprint generated-class " +
          "path, or a short native type name ('StaticMeshComponent', " +
          "'PointLightComponent'). Must resolve to a concrete (non-abstract) " +
          "UActorComponent subclass.",
      },
      name: {
        type: "string",
        description:
          "Optional UObject name for the new component. Auto-generated from " +
          "the class name when omitted. A name that collides with an existing " +
          "component on the actor returns name_conflict (NewObject would " +
          "otherwise fatal-assert).",
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
