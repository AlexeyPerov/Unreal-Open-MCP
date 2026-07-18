import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.3 — first mutating actor tool. Spawns an actor in the current editor
// level from a native class path or a Blueprint asset path, optionally setting
// label / location / rotation / parent attachment. Returns the new actor's
// ActorData (label, name, class, path, transform, components) so the agent can
// chain actor_find / future modify / tree tools without a second call.
//
// Intentional deltas vs Unity's gameobject-create:
//   - `classPath` replaces Unity's fixed `primitive_type` enum. Unreal spawns
//     from a native class (`/Script/Engine.PointLight`, `StaticMeshActor`) or a
//     Blueprint generated-class path (`/Game/BP/BP_Foo.BP_Foo_C`).
//   - `parent` is a single string ref (label → name → path), not Unity's
//     `parent_path` hierarchy string. Attachment targets the parent actor's
//     root via Unreal's AttachToActor.
//   - Transform is `{location{x,y,z}, rotation{pitch,yaw,roll}, scale}` (Unreal
//     conventions); rotation is optional pitch/yaw/roll in degrees.
//   - Undo is `FScopedTransaction` (Unreal), not Unity's
//     Undo.RegisterCreatedObjectUndo.
//
// Gate: `paths_hint` + `gate` are accepted on the schema for forward-compat but
// NOT enforced until P3.5 (documented P2.3 deferral). Route: live
// (POST /tools/unreal_open_mcp_actor_create). Mutating.
export const actorCreate: Tool = {
  name: "unreal_open_mcp_actor_create",
  description:
    "Spawn a new actor in the current editor level from a native class path " +
    "(e.g. '/Script/Engine.PointLight', 'StaticMeshActor') or a Blueprint " +
    "asset path ('/Game/BP/BP_Foo.BP_Foo_C'). Optionally set the actor label, " +
    "world location, world rotation, and a parent actor to attach to. Returns " +
    "the new actor's ActorData (label, name, class, path, transform, and a " +
    "short components array) so you can chain actor_find / future modify / " +
    "tree tools without a second call. Mutating: wrapped in FScopedTransaction " +
    "for editor Undo; marks the level package dirty. `paths_hint` + `gate` are " +
    "accepted for forward-compat but gate enforcement is deferred (no-op in " +
    "P2). Error codes: missing_parameter (classPath absent), class_not_found " +
    "(path did not resolve), invalid_parameter (resolved class is not an Actor " +
    "or is abstract), parent_not_found (parent ref did not resolve; nothing " +
    "spawned), spawn_failed (SpawnActor returned null), no_editor_world. " +
    "Prefer this over raw invoke_method SpawnActor — structured output + " +
    "addressing parity with the rest of the actor family.",
  inputSchema: {
    type: "object",
    required: ["classPath"],
    properties: {
      classPath: {
        type: "string",
        description:
          "Native class or Blueprint asset path/name to spawn. Accepts a soft " +
          "class path ('/Script/Engine.PointLight', '/Game/BP/BP_Foo.BP_Foo_C'), " +
          "a Blueprint asset path (its generated class is used), or a short " +
          "native type name ('StaticMeshActor', 'PointLight'). Must resolve to " +
          "a concrete (non-abstract) Actor subclass.",
      },
      name: {
        type: "string",
        description:
          "Actor label (editor-visible friendly name). Auto-generated when " +
          "omitted. When supplied, a colliding label is de-duplicated so it " +
          "stays unambiguous to later actor lookups.",
      },
      location: {
        type: "object",
        description:
          "World location {x,y,z}. Defaults to {0,0,0}. Missing axes fall back " +
          "to 0.",
        properties: {
          x: { type: "number" },
          y: { type: "number" },
          z: { type: "number" },
        },
      },
      rotation: {
        type: "object",
        description:
          "World rotation {pitch,yaw,roll} in degrees. Defaults to {0,0,0}. " +
          "Missing axes fall back to 0.",
        properties: {
          pitch: { type: "number" },
          yaw: { type: "number" },
          roll: { type: "number" },
        },
      },
      parent: {
        type: "string",
        description:
          "Optional actor ref (label → name → path) to attach the new actor " +
          "to. The parent is resolved BEFORE spawning, so a bad ref returns " +
          "parent_not_found with nothing spawned (no orphan actor). " +
          "Attachment keeps the world transform (KeepWorldTransform). When the " +
          "spawned class has no root component the attach silently no-ops and " +
          "the result carries a `warning` field.",
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
