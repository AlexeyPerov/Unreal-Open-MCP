import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.4 — actor modify. Mutating: writes reflected FProperty values on resolved
// actor(s) via FProperty reflection + FJsonObjectConverter. Two targeting
// shapes: a single `actor` ref (label → name → path) or a batch `actors` array
// that fans the same `properties` bag out to each target.
//
// Intentional deltas vs Unity's gameobject-modify:
//   - A single flat `properties` object replaces Unity's three-surface RFC 7396
//     form (gameObjectDiffs / pathPatchesPerGameObject / jsonPatchesPerGameObject).
//     The three-surface form is a Unity-specific convenience for its
//     Component/SerializedObject model; Unreal's FProperty reflection + transform
//     shortcuts cover the same ground with one bag in P2.
//   - Transform shortcuts (location/rotation/scale) live INSIDE `properties`
//     and are routed to the actor transform APIs (SetActorLocation etc.) — they
//     are not top-level args. An actor transform is not a single writable
//     FProperty, so they are special-cased terminally in the bridge helper.
//   - Reflection writes go through FProperty (Unreal), not System.Reflection
//     (Unity). Enum writes are by name string, not int index.
//   - Partial success is the norm: per-field errors accumulate in `errors[]`
//     and do NOT abort the batch. A bad field name or a read-only property
//     surfaces as an error string while the rest of the bag still applies.
//
// Gate: `paths_hint` + `gate` are accepted on the schema for forward-compat but
// NOT enforced until P3.5 (documented P2.4 deferral). Route: live
// (POST /tools/unreal_open_mcp_actor_modify). Mutating.
export const actorModify: Tool = {
  name: "unreal_open_mcp_actor_modify",
  description:
    "Write reflected properties on one or more actors in the current editor " +
    "level. Pass a single `actor` ref (label → name → path) or a batch " +
    "`actors` array; both apply the same `properties` bag. The bag is a flat " +
    "name → value object: transform shortcuts `location` {x,y,z}, `rotation` " +
    "{pitch,yaw,roll}, `scale` {x,y,z} route to the actor transform APIs; " +
    "every other key is written by name through FProperty reflection " +
    "(bool/int/float/string/vector/rotator/color/enum-by-name). Mutating: " +
    "wrapped in FScopedTransaction for editor Undo; marks the level package " +
    "dirty. Partial success is the norm — per-field errors accumulate in " +
    "`errors[]` and do NOT abort the batch (one bad field name does not skip " +
    "the rest). `paths_hint` + `gate` are accepted for forward-compat but " +
    "gate enforcement is deferred (no-op in P2). Error codes: " +
    "missing_parameter (no actor/actors or no properties), actor_not_found " +
    "(a ref did not resolve; nothing mutated), no_editor_world. Result: " +
    "{ applied: number, actors: [{ label, path, applied, actor }], " +
    "errors?: string[] }. Prefer this over raw invoke_method SetActor* — " +
    "structured output + addressing parity with the rest of the actor family.",
  inputSchema: {
    type: "object",
    properties: {
      actor: {
        type: "string",
        description:
          "Single actor ref (label → name → path). Mutually informative with " +
          "`actors`; pass one of the two.",
      },
      actors: {
        type: "array",
        items: { type: "string" },
        description:
          "Batch: array of actor refs to apply the same `properties` bag to. " +
          "Each target is resolved BEFORE the transaction opens, so one bad " +
          "ref returns actor_not_found with nothing mutated (no partial batch).",
      },
      properties: {
        type: "object",
        description:
          "Flat name → value bag. Transform shortcuts (location/rotation/scale) " +
          "route to the actor transform APIs; all other keys are written by " +
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
