import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.5 — actor duplicate. Mutating: clones an actor in the current editor
// level via SpawnActor with the source as the template (SpawnParams.Template),
// which copies the source's component state into the clone. Optionally renames
// the clone and attaches it to a parent actor.
//
// Intentional deltas vs Unity's gameobject-duplicate:
//   - Duplication uses SpawnActor-from-template (the behavior reference's path)
//     instead of Unity's PrefabUtility.InstantiatePrefab / Instantiate. This
//     stays headless-safe (no EditorActorSubsystem dependency) and copies
//     component state into the clone.
//   - The clone's label is de-duplicated via SetActorLabelUnique so it stays
//     unambiguous to later actor lookups (the default copy is the source label
//     verbatim, which would collide).
//   - An optional world-space `offset` nudges the clone off the source so it is
//     visible in the viewport without a second actor_modify call.
//
// Gate: `paths_hint` + `gate` are accepted on the schema for forward-compat but
// NOT enforced until P3.5 (documented P2.5 deferral). Route: live
// (POST /tools/unreal_open_mcp_actor_duplicate). Mutating.
export const actorDuplicate: Tool = {
  name: "unreal_open_mcp_actor_duplicate",
  description:
    "Duplicate an actor in the current editor level. Clones the source via " +
    "SpawnActor with the source as the template (component state is copied " +
    "into the clone). Optionally renames the clone and attaches it to a parent " +
    "actor. The clone's label is de-duplicated (SetActorLabelUnique) so it " +
    "stays unambiguous to later actor lookups. The source and optional parent " +
    "are resolved BEFORE the transaction opens, so a miss returns " +
    "actor_not_found / parent_not_found with nothing spawned (no orphan). " +
    "Returns the clone's ActorData (label, name, class, path, transform, " +
    "components) so you can chain modify / tree tools without a second call. " +
    "Mutating: wrapped in FScopedTransaction for editor Undo; marks the level " +
    "package dirty. `paths_hint` + `gate` are accepted for forward-compat but " +
    "gate enforcement is deferred (no-op in P2). Error codes: missing_parameter " +
    "(actor absent), actor_not_found (source ref did not resolve), " +
    "parent_not_found (parent ref did not resolve; nothing spawned), " +
    "spawn_failed (SpawnActor returned null), no_editor_world. Result: " +
    "{ actor: ActorData, warning?: string }. Prefer this over raw invoke_method " +
    "SpawnActor — structured output + addressing parity with the rest of the " +
    "actor family.",
  inputSchema: {
    type: "object",
    required: ["actor"],
    properties: {
      actor: {
        type: "string",
        description:
          "Source actor ref (label → name → path) to duplicate. Resolved " +
          "before spawning, so a bad ref returns actor_not_found with nothing " +
          "spawned.",
      },
      name: {
        type: "string",
        description:
          "Optional label for the clone. Auto-de-duplicated on collision so it " +
          "stays unambiguous. When omitted, the clone keeps the source label " +
          "(also de-duplicated).",
      },
      parent: {
        type: "string",
        description:
          "Optional actor ref (label → name → path) to attach the clone to. " +
          "Resolved before spawning; a bad ref returns parent_not_found with " +
          "nothing spawned. When the clone has no root component the attach " +
          "silently no-ops and the result carries a `warning` field.",
      },
      offset: {
        type: "object",
        description:
          "Optional world-space offset added to the clone's location " +
          "({x,y,z}). Defaults to {0,0,0} (clone stacks on the source). Use a " +
          "small +Z nudge to keep the clone visible in the viewport.",
        properties: {
          x: { type: "number" },
          y: { type: "number" },
          z: { type: "number" },
        },
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope (forward-compat) — level/map content path the " +
          "duplicate is scoped to. Accepted but NOT enforced until the gate " +
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
