import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// Blueprint spawn. Instance a compiled Actor Blueprint's GeneratedClass into
// the current editor level via UWorld::SpawnActor (headless-safe — NOT the
// viewport-aware UEditorActorSubsystem::SpawnActorFromClass that crashes under
// -nullrhi / Automation). `path` is the Blueprint asset object path
// (package-path form also accepted — resolved in-memory first, then loaded).
//
// This closes the Phase 6 create -> edit -> compile -> spawn loop: a Blueprint
// created via blueprint_create, structurally edited via the add/modify tools,
// and landed on a GeneratedClass via blueprint_compile is now placeable into
// the editor world. The expected agent loop is:
//   blueprint_create -> (add_variable / add_component / add_function /
//   add_event)* -> blueprint_compile -> blueprint_spawn -> blueprint_get /
//   actor_find.
//
// Compile-first: spawn requires a non-null GeneratedClass (the result of a
// compile). A Blueprint that was never compiled — or whose GeneratedClass was
// invalidated — reports not_compiled with a message pointing at
// blueprint_compile. Non-Actor Blueprints (a Blueprint Function Library / an
// Object Blueprint) report not_actor_blueprint. No editor world -> no_editor_world.
//
// MVP scope: rotation is fixed at identity; no PIE-only spawn path, no
// multiplayer, no parent attachment (use actor_create / actor_set_parent for
// those). Optional `location` ({x,y,z}, default origin) + `name` (actor label,
// de-duplicated via SetActorLabelUnique). The spawn is wrapped in
// FScopedTransaction for editor Undo and marks the level package dirty.
//
// Mutating: runs the full gate path (checkpoint -> spawn -> validate -> delta);
// `paths_hint` MUST list the Blueprint package path (e.g.
// ['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set
// gate:"off" to bypass.
//
// Fidelity: greenfield. No Unity Blueprint twin (loose analogy only: Unity's
// prefab instantiate workflow — different engine, no shared code). Behavior
// reference (read-only): Unreal-MCP's blueprint-spawn for the
// World->SpawnActor headless-safe path + the GeneratedClass-present +
// IsChildOf(AActor) guards.
//
// Intentional deltas vs Unreal-MCP:
//   - Canonical MCP envelope + gate summary on spawn.
//   - Structured not_compiled / not_actor_blueprint codes (Unreal-MCP folds
//     both into a generic spawn-failure message).
//
// Route: live (POST /tools/unreal_open_mcp_blueprint_spawn). Mutating.
export const blueprintSpawn: Tool = {
  name: "unreal_open_mcp_blueprint_spawn",
  description:
    "Spawn an instance of a compiled Actor Blueprint into the current editor " +
    "level. `path` is the Blueprint asset object path (package-path form also " +
    "accepted — resolved in-memory first, then loaded). Spawn uses " +
    "UWorld::SpawnActor (headless-safe — NOT the viewport-aware editor " +
    "subsystem that crashes under -nullrhi). Compile-first: spawn requires a " +
    "non-null GeneratedClass — a Blueprint that was never compiled reports " +
    "not_compiled (run blueprint_compile first); a non-Actor Blueprint reports " +
    "not_actor_blueprint. Optional `location` ({x,y,z}, default origin) and " +
    "`name` (actor label, de-duplicated). Rotation is fixed at identity in the " +
    "MVP. This closes the Phase 6 loop: blueprint_create -> (add_variable / " +
    "add_component / add_function / add_event)* -> blueprint_compile -> " +
    "blueprint_spawn -> blueprint_get / actor_find. Mutating: wrapped in " +
    "FScopedTransaction for editor Undo; marks the level package dirty; runs " +
    "the full gate path (checkpoint -> spawn -> validate -> delta); " +
    "`paths_hint` MUST list the Blueprint package path (e.g. " +
    "['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set " +
    "gate:\"off\" to bypass. Result shape: { actor (label), name, class, path, " +
    "location:{x,y,z} }. Error codes: missing_parameter (path absent), " +
    "blueprint_not_found (no Blueprint at path), not_compiled (no " +
    "GeneratedClass — compile first), not_actor_blueprint (GeneratedClass is " +
    "not AActor-derived), no_editor_world (no level open), spawn_failed " +
    "(SpawnActor returned null), invalid_parameter (malformed body).",
  inputSchema: {
    type: "object",
    required: ["path", "paths_hint"],
    properties: {
      path: {
        type: "string",
        description:
          "Blueprint asset object path — an object path " +
          "('/Game/Mcp/BP_Thing.BP_Thing') or a package path " +
          "('/Game/Mcp/BP_Thing'). Resolved in-memory first (a Blueprint " +
          "created this session but not yet saved), then loaded.",
      },
      location: {
        type: "object",
        description:
          "World location {x,y,z} for the spawned actor. Defaults to {0,0,0}. " +
          "Missing axes fall back to 0.",
        properties: {
          x: { type: "number" },
          y: { type: "number" },
          z: { type: "number" },
        },
        additionalProperties: false,
      },
      name: {
        type: "string",
        description:
          "Actor label (editor-visible friendly name). Auto-generated when " +
          "omitted. When supplied, a colliding label is de-duplicated so it " +
          "stays unambiguous to later actor lookups.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — the Blueprint package path(s) the mutation is " +
          "scoped to, fed to the gate as the checkpoint + validate hint. " +
          "REQUIRED for mutating tools (the gate refuses an empty hint with " +
          "paths_hint_required; there is no whole-project fallback). Set " +
          "gate:\"off\" to bypass the gate and skip the hint.",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint -> spawn -> " +
          "validate -> delta and hard-fails on new Errors; warn commits the " +
          "spawn but surfaces new Errors as warnings; off skips the gate " +
          "entirely (paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};
