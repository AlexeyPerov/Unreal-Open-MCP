import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// Blueprint add variable. Adds a typed member variable via
// FBlueprintEditorUtils::AddMemberVariable using the §3.2 pin-type forward
// mapping (the forward twin of the PinTypeToString reverse map blueprint_get
// already uses). `path` is the Blueprint asset object path (package-path form
// also accepted — resolved in-memory first, then loaded). `name` is the new
// member variable name. `type` is a pin-type token: a primitive
// (bool/int/int64/byte/float/double/string/name/text), a math struct
// (vector/vector2d/rotator/transform/color), or a resolvable object/struct
// path. Optional `is_array` wraps the type in an array container. Optional
// `default_value` is stored on the variable descriptor in UE text format and
// only takes effect after a compile lands the property on the generated class.
//
// Member variables are NOT local function variables — they are the class-level
// properties an agent sets up before compile/spawn. Chain blueprint_compile
// after a batch of adds so the generated class + CDO reflect them, then
// blueprint_set_default to seed their values on the CDO.
//
// Guards (each maps to a structured error code):
//   - the name must not collide across the NewVariables list, the SCS, and the
//     parent class's properties (name_collision) — those namespaces share the
//     generated class's property namespace and would otherwise only fail at
//     compile
//   - an unresolvable type token is rejected with invalid_type
//
// Mutating: runs the full gate path (checkpoint -> add -> validate -> delta);
// `paths_hint` MUST list the Blueprint package path (e.g.
// ['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set
// gate:"off" to bypass. Chain into blueprint_get to confirm the variable now
// appears (with its type + isArray flag + default).
//
// Fidelity: greenfield. No Unity Blueprint / prefab-graph twin. Behavior
// reference (read-only): Unreal-MCP's blueprint-add-variable for the pin-type
// forward map + AddMemberVariable + the cross-namespace name checks.
//
// Intentional deltas vs Unreal-MCP:
//   - Canonical MCP envelope + gate summary on add.
//   - snake_case arg names (`is_array`, `default_value`); the bridge accepts
//     the camelCase aliases too.
//
// Route: live (POST /tools/unreal_open_mcp_blueprint_add_variable). Mutating.
export const blueprintAddVariable: Tool = {
  name: "unreal_open_mcp_blueprint_add_variable",
  description:
    "Add a typed member variable to a Blueprint via " +
    "FBlueprintEditorUtils::AddMemberVariable. `path` is the Blueprint asset " +
    "object path (package-path form also accepted — resolved in-memory first, " +
    "then loaded). `name` is the new member variable name (Kismet name " +
    "validator rules). `type` is a pin-type token: a primitive " +
    "(bool/int/int64/byte/float/string/name/text), a math struct " +
    "(vector/vector2d/rotator/transform/color), or a resolvable object/struct " +
    "path. Optional `is_array` wraps the type in an array. Optional " +
    "`default_value` (UE text format) is stored on the variable descriptor and " +
    "only takes effect after blueprint_compile lands the property on the " +
    "generated class. Member variables are class-level properties (not local " +
    "function vars). Mutating: runs the full gate path (checkpoint -> add -> " +
    "validate -> delta); `paths_hint` MUST list the Blueprint package path " +
    "(e.g. ['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set " +
    "gate:\"off\" to bypass. Result shape: { variable, type }. Error codes: " +
    "missing_parameter (path/name/type absent), blueprint_not_found (no " +
    "Blueprint at path), invalid_name (name failed the Kismet name validator), " +
    "name_collision (name already used by a member variable, an SCS component, " +
    "or a parent-class property — those namespaces share the generated " +
    "class's property namespace and would otherwise only fail at compile), " +
    "invalid_type (type token did not resolve to a primitive/math-struct/" +
    "object-or-struct path), add_failed (AddMemberVariable refused the " +
    "name/type — the type may be unsupported as a member variable), " +
    "invalid_parameter (malformed body).",
  inputSchema: {
    type: "object",
    required: ["path", "name", "type", "paths_hint"],
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
          "New member variable name (Kismet name validator rules). Must not " +
          "collide with an existing member variable, an SCS component, or a " +
          "parent-class property — those namespaces share the generated " +
          "class's property namespace and would otherwise only fail at compile.",
      },
      type: {
        type: "string",
        description:
          "Pin-type token. Primitives: bool, int, int64, byte, float, double, " +
          "string, name, text. Math structs: vector, vector2d, rotator, " +
          "transform, color (linearcolor). Anything else is resolved as an " +
          "object/struct path (e.g. '/Script/Engine.Actor' for an object " +
          "soft-class ref, or a UScriptStruct path).",
      },
      is_array: {
        type: "boolean",
        default: false,
        description:
          "Wrap the type in an array container. Defaults to false. The " +
          "camelCase alias `isArray` is also accepted by the bridge.",
      },
      default_value: {
        type: "string",
        description:
          "Optional default value in UE text format (numbers, " +
          "'(X=1,Y=2,Z=3)' for structs, asset paths for object refs). Stored " +
          "on the variable descriptor; only takes effect after " +
          "blueprint_compile lands the property on the generated class. The " +
          "camelCase alias `defaultValue` is also accepted by the bridge.",
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
          "Gate mode — enforce (default) runs checkpoint -> add -> validate " +
          "-> delta and hard-fails on new Errors; warn commits the mutation " +
          "but surfaces new Errors as warnings; off skips the gate entirely " +
          "(paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};
