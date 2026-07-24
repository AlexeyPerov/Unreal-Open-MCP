import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// Blueprint modify variable. Rename and/or retype an existing member variable.
// `path` is the Blueprint asset object path (package-path form also accepted —
// resolved in-memory first, then loaded). `name` is the existing member
// variable. At least one of `new_name` / `new_type` is required. `new_type`
// re-parses a pin-type token (with an optional `is_array`); `new_name` renames.
//
// Validate-before-mutate ordering: the rename is validated (well-formed + no
// collision) BEFORE the retype commits, so a colliding or ill-formed new_name
// never leaves a partial mutation (retype landed, rename failed).
// ChangeMemberVariableType commits immediately; RenameMemberVariable is void
// and does no collision check of its own — renaming onto an existing name
// would silently produce duplicate member names that break a later compile —
// both are pre-checked here. This is the critical invariant the tool pins.
//
// Mutating: runs the full gate path (checkpoint -> modify -> validate ->
// delta); `paths_hint` MUST list the Blueprint package path (e.g.
// ['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set
// gate:"off" to bypass. Chain into blueprint_get to confirm the rename/retype
// landed (old name gone / new name present / type updated).
//
// Fidelity: greenfield. No Unity Blueprint / prefab-graph twin. Behavior
// reference (read-only): Unreal-MCP's blueprint-modify-variable for the
// validate-before-retype ordering fix + ChangeMemberVariableType /
// RenameMemberVariable.
//
// Intentional deltas vs Unreal-MCP:
//   - Canonical MCP envelope + gate summary on modify.
//   - snake_case arg names (`new_name`, `new_type`, `is_array`); the bridge
//     accepts the camelCase aliases too.
//
// Route: live (POST /tools/unreal_open_mcp_blueprint_modify_variable).
// Mutating.
export const blueprintModifyVariable: Tool = {
  name: "unreal_open_mcp_blueprint_modify_variable",
  description:
    "Rename and/or retype an existing Blueprint member variable. `path` is " +
    "the Blueprint asset object path (package-path form also accepted — " +
    "resolved in-memory first, then loaded). `name` is the existing member " +
    "variable. At least one of `new_name` / `new_type` is required. `new_type` " +
    "re-parses a pin-type token (see blueprint_add_variable) with an optional " +
    "`is_array`; `new_name` renames. Validate-before-mutate: the rename is " +
    "validated (well-formed + no collision) BEFORE the retype commits, so a " +
    "colliding or ill-formed new_name never leaves a partial mutation. " +
    "Mutating: runs the full gate path (checkpoint -> modify -> validate -> " +
    "delta); `paths_hint` MUST list the Blueprint package path (e.g. " +
    "['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set " +
    "gate:\"off\" to bypass. Result shape: { variable } (the effective name " +
    "after a rename). Error codes: missing_parameter (path/name absent, or " +
    "neither new_name nor new_type), blueprint_not_found (no Blueprint at " +
    "path), variable_not_found (name names no member variable), invalid_name " +
    "(new_name failed the Kismet name validator), name_collision (new_name " +
    "already used by a member variable, an SCS component, or a parent-class " +
    "property), invalid_type (new_type token did not resolve), " +
    "invalid_parameter (malformed body).",
  inputSchema: {
    type: "object",
    required: ["path", "name", "paths_hint"],
    properties: {
      path: {
        type: "string",
        description:
          "Blueprint asset object path — an object path " +
          "('/Game/Mcp/BP_Thing.BP_Thing') or a package path " +
          "('/Game/Mcp/BP_Thing'). Resolved in-memory first (a Blueprint " +
          "created this session but not yet saved), then loaded.",
      },
      name: {
        type: "string",
        description:
          "Existing member variable name (the `name` field on a variable " +
          "entry returned by blueprint_get).",
      },
      new_name: {
        type: "string",
        description:
          "Optional new variable name (Kismet name validator rules). Must not " +
          "collide with an existing member variable, an SCS component, or a " +
          "parent-class property. Validated BEFORE a new_type commits, so a " +
          "collision never leaves a partial retype. At least one of new_name / " +
          "new_type is required. The camelCase alias `newName` is also " +
          "accepted by the bridge.",
      },
      new_type: {
        type: "string",
        description:
          "Optional new pin-type token (see blueprint_add_variable). Applied " +
          "with the optional `is_array`. At least one of new_name / new_type " +
          "is required. The camelCase alias `newType` is also accepted by the " +
          "bridge.",
      },
      is_array: {
        type: "boolean",
        default: false,
        description:
          "Array flag applied together with `new_type`. Ignored when new_type " +
          "is absent. Defaults to false. The camelCase alias `isArray` is " +
          "also accepted by the bridge.",
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
          "Gate mode — enforce (default) runs checkpoint -> modify -> " +
          "validate -> delta and hard-fails on new Errors; warn commits the " +
          "mutation but surfaces new Errors as warnings; off skips the gate " +
          "entirely (paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};
