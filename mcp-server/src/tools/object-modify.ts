import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.4 — object modify. Mutating: writes reflected FProperty values on ANY
// resolved UObject (actor, component, asset instance) via the same FProperty
// reflection path as actor_modify. The difference from actor_modify is the
// addressing surface: `object` resolves through ResolveObject (actor → loaded →
// soft-path → StaticLoadObject), so the same tool reaches components and asset
// instances that actor_modify's actor-only sweep cannot.
//
// Intentional deltas vs Unity's object-modify:
//   - `object` ref string replaces Unity's instance_id / asset_path resolver
//     pair (Unreal has no global int instance id). A single ref covers actors,
//     in-memory UObjects, and on-disk assets.
//   - The `properties` bag replaces Unity's `fields` array of {name, value}
//     patches. Same flat name → value shape; different container (object vs
//     array) to match actor_modify and keep the mutate family uniform.
//   - Reflection writes go through FProperty + FJsonObjectConverter (Unreal),
//     not System.Reflection + ConvertValue (Unity). The writable-property
//     gate (CPF_Edit && !CPF_EditConst, or CPF_BlueprintVisible &&
//     !CPF_BlueprintReadOnly) replaces Unity's public-field/property + setter
//     check; read-only properties are rejected explicitly with an error in
//     `errors[]`, not silently no-op'd.
//   - Transform shortcuts (location/rotation/scale on actors; relativeLocation/
//     relativeRotation/relativeScale3D on scene components) are honored
//     automatically based on the resolved object's type.
//
// Gate: `paths_hint` + `gate` are accepted on the schema for forward-compat but
// NOT enforced until P3.5 (documented P2.4 deferral). Route: live
// (POST /tools/unreal_open_mcp_object_modify). Mutating.
export const objectModify: Tool = {
  name: "unreal_open_mcp_object_modify",
  description:
    "Write reflected properties on any UObject — an actor, a component, or an " +
    "asset instance — resolved by `object` (actor label/name/path, in-memory " +
    "object path, or asset soft path). The `properties` bag is a flat " +
    "name → value object written by name through FProperty reflection " +
    "(bool/int/float/string/vector/rotator/color/enum-by-name). Transform " +
    "shortcuts are honored automatically: location/rotation/scale when the " +
    "object is an actor, relativeLocation/relativeRotation/relativeScale3D " +
    "when it is a scene component. Mutating: wrapped in FScopedTransaction " +
    "for editor Undo; marks the object's package dirty. Partial success is " +
    "the norm — per-field errors accumulate in `errors[]` and do NOT abort " +
    "the batch. Read-only properties are rejected explicitly (an error in " +
    "`errors[]`, not a silent no-op). `paths_hint` + `gate` are accepted " +
    "for forward-compat but gate enforcement is deferred (no-op in P2). " +
    "Error codes: missing_parameter (no object or no properties), " +
    "object_not_found (ref did not resolve; nothing mutated), no_editor_world. " +
    "Result: { applied: number, name, class, path, errors?: string[] }. " +
    "Use this for components/assets that actor_modify's actor-only sweep " +
    "cannot reach; prefer actor_modify for actor transforms.",
  inputSchema: {
    type: "object",
    properties: {
      object: {
        type: "string",
        description:
          "Object ref — an actor label/name/path (takes precedence so " +
          "object-* can target scene actors), an in-memory UObject path " +
          "(GetPathName), or an asset soft path ('/Game/.../Foo.Foo'). " +
          "Resolved via ResolveObject (actor → loaded → soft-path → " +
          "StaticLoadObject).",
      },
      properties: {
        type: "object",
        description:
          "Flat name → value bag written by name through FProperty " +
          "reflection. Transform shortcuts are honored based on the " +
          "resolved object's type (actor vs scene component). An empty " +
          "object {} is valid (applies nothing, useful to probe resolution).",
        additionalProperties: {},
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope (forward-compat) — asset/object path the write is " +
          "scoped to. Accepted but NOT enforced until the gate lands (P3.5).",
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
