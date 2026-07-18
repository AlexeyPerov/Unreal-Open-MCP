import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.5 — actor set-parent (reparent). Mutating: attaches the child actor's root
// component to the parent actor via AttachToActor. Cycle-safe — a self-parent or
// a parent that is already a descendant of the child is rejected up front via
// IsAttachedTo before the attach runs (the engine would otherwise silently drop
// a cycle-forming attach).
//
// Intentional deltas vs Unity's gameobject-set-parent:
//   - Addressing is a single string ref per side (label → name → path), not
//     Unity's instance_id / path / name trio — Unreal has no global instance id.
//   - Attachment targets the actors' root components (AttachToActor), not a
//     direct transform reparent (Unity's Undo.SetTransformParent). The
//     KeepWorldTransform rules flag (default true) is the analog of Unity's
//     world_position_stays.
//   - Cycle detection uses Unreal's IsAttachedTo (walks the attachment ancestry)
//     instead of Unity's upward parent-chain walk.
//
// Gate: `paths_hint` + `gate` are accepted on the schema for forward-compat but
// NOT enforced until P3.5 (documented P2.5 deferral). Route: live
// (POST /tools/unreal_open_mcp_actor_set_parent). Mutating.
export const actorSetParent: Tool = {
  name: "unreal_open_mcp_actor_set_parent",
  description:
    "Reparent an actor under another actor in the current editor level. " +
    "Attaches the child actor's root component to the parent actor via " +
    "AttachToActor; keepWorldTransform (default true) preserves the world " +
    "transform across the reparent. Cycle-safe — a self-parent or a parent " +
    "that is already a descendant of the child is rejected up front before " +
    "the attach runs (the engine would otherwise silently drop it). The child " +
    "and parent are resolved BEFORE the transaction opens, so a miss returns " +
    "actor_not_found / parent_not_found with nothing mutated. Mutating: " +
    "wrapped in FScopedTransaction for editor Undo; marks the level package " +
    "dirty. `paths_hint` + `gate` are accepted for forward-compat but gate " +
    "enforcement is deferred (no-op in P2). Error codes: missing_parameter " +
    "(actor or parent absent), actor_not_found (child ref did not resolve), " +
    "parent_not_found (parent ref did not resolve), would_create_cycle " +
    "(self-parent or parent is a descendant of child), missing_root_component " +
    "(either side has no root component), attach_failed (engine rejected the " +
    "attach), no_editor_world. Result: { actor: ActorData }. Prefer this over " +
    "raw invoke_method AttachToActor — structured output + addressing parity " +
    "with the rest of the actor family.",
  inputSchema: {
    type: "object",
    required: ["actor", "parent"],
    properties: {
      actor: {
        type: "string",
        description:
          "Child actor ref (label → name → path) to reparent. Resolved before " +
          "the parent, so a bad child ref returns actor_not_found with nothing " +
          "mutated.",
      },
      parent: {
        type: "string",
        description:
          "New parent actor ref (label → name → path). Resolved before the " +
          "transaction opens; a bad ref returns parent_not_found with nothing " +
          "mutated.",
      },
      keepWorldTransform: {
        type: "boolean",
        default: true,
        description:
          "Preserve the child's world transform across the reparent (default " +
          "true, KeepWorldTransform). Set false to keep the local-space " +
          "transform instead (KeepRelativeTransform).",
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
