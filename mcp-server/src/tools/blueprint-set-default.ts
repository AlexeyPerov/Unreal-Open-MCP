import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// Blueprint set default. Writes a Class Default Object (CDO) property via the
// property's own text importer (ImportText_Direct), bracketed with
// Pre/PostEditChangeProperty so an open Details panel / property-change
// observers refresh. `path` is the Blueprint asset object path (package-path
// form also accepted — resolved in-memory first, then loaded). `property` is a
// property name on the generated class; `value` is the value in UE text format
// (numbers, '(X=1,Y=2,Z=3)' for structs, asset paths for object refs).
//
// This changes the CLASS DEFAULT, so it affects newly-spawned instances only —
// not actors already placed in a level. To change a placed actor, use
// actor_modify instead.
//
// Compile-first: a member variable added via blueprint_add_variable is NOT yet
// a property on the generated class until a compile lands it. set_default on
// such a property reports property_not_found with a message that points at
// blueprint_compile. The expected loop is: blueprint_add_variable ->
// blueprint_compile -> blueprint_set_default.
//
// Mutating: runs the full gate path (checkpoint -> write -> validate -> delta);
// `paths_hint` MUST list the Blueprint package path (e.g.
// ['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set
// gate:"off" to bypass.
//
// Fidelity: greenfield. No Unity Blueprint / prefab-graph twin (loose analogy:
// ScriptableObject field writes / the object_modify workflow). Behavior
// reference (read-only): Unreal-MCP's blueprint-set-default for the CDO
// ImportText_Direct + Pre/PostEditChangeProperty write protocol + the
// compile-first property_not_found message.
//
// Intentional deltas vs Unreal-MCP:
//   - Canonical MCP envelope + gate summary on write.
//
// Route: live (POST /tools/unreal_open_mcp_blueprint_set_default). Mutating.
export const blueprintSetDefault: Tool = {
  name: "unreal_open_mcp_blueprint_set_default",
  description:
    "Set a default property value on a Blueprint's Class Default Object " +
    "(CDO). `path` is the Blueprint asset object path (package-path form also " +
    "accepted — resolved in-memory first, then loaded). `property` is a " +
    "property name on the generated class. `value` is the value in UE text " +
    "format — parsed with the property's own text importer, so it accepts " +
    "numbers, '(X=1,Y=2,Z=3)' for structs, and asset paths for object refs. " +
    "This changes the CLASS DEFAULT, so it affects newly-spawned instances " +
    "only — not actors already placed in a level (use actor_modify for a " +
    "placed actor). Compile-first: a member variable added via " +
    "blueprint_add_variable is NOT yet a property on the generated class until " +
    "blueprint_compile lands it — the expected loop is add -> compile -> " +
    "set_default. Mutating: runs the full gate path (checkpoint -> write -> " +
    "validate -> delta); `paths_hint` MUST list the Blueprint package path " +
    "(e.g. ['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set " +
    "gate:\"off\" to bypass. Result shape: { property, value }. Error codes: " +
    "missing_parameter (path/property absent), blueprint_not_found (no " +
    "Blueprint at path), no_generated_class (Blueprint has no GeneratedClass " +
    "— compile first), property_not_found (property absent on the generated " +
    "class — if you just added it via blueprint_add_variable, run " +
    "blueprint_compile first), import_failed (the value could not be parsed " +
    "for the property's type), invalid_parameter (malformed body).",
  inputSchema: {
    type: "object",
    required: ["path", "property", "value", "paths_hint"],
    properties: {
      path: {
        type: "string",
        description:
          "Blueprint asset object path — an object path " +
          "('/Game/Mcp/BP_Thing.BP_Thing') or a package path " +
          "('/Game/Mcp/BP_Thing'). Resolved in-memory first (a Blueprint " +
          "created this session but not yet saved), then loaded.",
      },
      property: {
        type: "string",
        description:
          "CDO property name on the generated class. A member variable added " +
          "via blueprint_add_variable only becomes a property after " +
          "blueprint_compile — set_default on an uncompiled new variable " +
          "reports property_not_found (the message points at blueprint_compile).",
      },
      value: {
        type: "string",
        description:
          "Value in UE text format — parsed with the property's own text " +
          "importer. Numbers ('42', '1.5'), struct literals " +
          "('(X=1,Y=2,Z=3)'), and asset paths for object refs are accepted.",
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
          "Gate mode — enforce (default) runs checkpoint -> write -> " +
          "validate -> delta and hard-fails on new Errors; warn commits the " +
          "mutation but surfaces new Errors as warnings; off skips the gate " +
          "entirely (paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};
