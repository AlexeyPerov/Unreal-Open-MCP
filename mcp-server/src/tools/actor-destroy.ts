import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.5 — actor destroy. Mutating: destroys one or more actors in the current
// editor level via EditorDestroyActor (the editor path — records into the
// transaction buffer, updates the level's actor list, and fires the editor's
// selection/outliner refresh hooks). Supports a single `actor` or a batch
// `actors` array; the batch resolves every target BEFORE the transaction opens
// so one bad ref returns actor_not_found with nothing destroyed (no partial
// batch).
//
// Intentional deltas vs Unity's gameobject-destroy:
//   - Single + batch in one tool. Unity's gameobject-destroy is single-target
//     only (batching happens at the separate batch-execute level). The Unreal
//     family folds batch into the tool itself because Unreal agents frequently
//     tear down a group of actors at once.
//   - Destroy is the editor path (EditorDestroyActor with bShouldModifyLevel=
//     true) rather than the runtime World->DestroyActor, so the editor's
//     outliner / selection state stays consistent.
//
// Gate: `paths_hint` + `gate` are accepted on the schema for forward-compat but
// NOT enforced until P3.5 (documented P2.5 deferral). Route: live
// (POST /tools/unreal_open_mcp_actor_destroy). Mutating.
export const actorDestroy: Tool = {
  name: "unreal_open_mcp_actor_destroy",
  description:
    "Destroy one or more actors in the current editor level. Pass a single " +
    "`actor` ref (label → name → path) or a batch `actors` array. Each target " +
    "is resolved BEFORE the transaction opens, so one bad ref returns " +
    "actor_not_found with nothing destroyed (no partial batch). Each destroy " +
    "goes through EditorDestroyActor (the editor path — records into the undo " +
    "buffer, updates the level's actor list, fires selection/outliner refresh). " +
    "Mutating: wrapped in FScopedTransaction for editor Undo; marks the level " +
    "package dirty. `paths_hint` + `gate` are accepted for forward-compat but " +
    "gate enforcement is deferred (no-op in P2). Error codes: missing_parameter " +
    "(no actor/actors), actor_not_found (a ref did not resolve; nothing " +
    "destroyed), destroy_failed (EditorDestroyActor returned false), " +
    "no_editor_world. Result: { destroyed: string[], count: number } (the " +
    "destroyed array holds each destroyed actor's label). Prefer this over " +
    "raw invoke_method DestroyActor — structured output + batch support + " +
    "addressing parity with the rest of the actor family.",
  inputSchema: {
    type: "object",
    properties: {
      actor: {
        type: "string",
        description:
          "Single actor ref (label → name → path) to destroy. Mutually " +
          "informative with `actors`; pass one of the two.",
      },
      actors: {
        type: "array",
        items: { type: "string" },
        description:
          "Batch: array of actor refs to destroy. Each target is resolved " +
          "BEFORE the transaction opens, so one bad ref returns actor_not_found " +
          "with nothing destroyed (no partial batch).",
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
