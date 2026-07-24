import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// Blueprint add component. Wraps the public Simple Construction Script surface
// (USimpleConstructionScript::CreateNode + AddChildNode / AddNode) so an agent
// can populate an Actor Blueprint's component graph without composing SCS calls
// by hand. `path` is the Blueprint asset object path (package-path form also
// accepted — resolved in-memory first, then loaded). `component_class` is a
// UActorComponent subclass path or short name (e.g.
// '/Script/Engine.StaticMeshComponent' or 'StaticMeshComponent'). `name` is the
// new component's variable name. Optional `parent_component` attaches the new
// node under an existing SCS scene-component node; omitted → added as a root
// node. After the add, MarkBlueprintAsStructurallyModified fires so a later
// compile rebuilds the CDO.
//
// Guards (each maps to a structured error code, never an engine assert):
//   - the component class must be a non-abstract, non-deprecated
//     UActorComponent subclass — SCS CreateNode -> NewObject fatally asserts on
//     an abstract class (e.g. '/Script/Engine.LightComponentBase'), so the
//     ClassFlags check runs BEFORE CreateNode
//   - the variable name must not collide across the SCS, the member-variable
//     list, or the parent class's properties (those namespaces share the
//     generated class's property namespace and would otherwise only fail at
//     compile)
//   - attachment is a scene-graph op, so both the new component and its parent
//     must be USceneComponents
//
// Mutating: runs the full gate path (checkpoint -> add -> validate -> delta);
// `paths_hint` MUST list the Blueprint package path (e.g.
// ['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set
// gate:"off" to bypass. Chain into blueprint_get to confirm the SCS now lists
// the component (and its attach parent).
//
// Fidelity: greenfield. No Unity Blueprint / prefab-graph twin. Behavior
// reference (read-only): Unreal-MCP's blueprint-add-component for the SCS
// create + attach API + the abstract-class guard + the cross-namespace name
// checks.
//
// Intentional deltas vs Unreal-MCP:
//   - Canonical MCP envelope + gate summary on add.
//   - snake_case arg names (`component_class`, `parent_component`); the bridge
//     also accepts the camelCase aliases.
//
// Route: live (POST /tools/unreal_open_mcp_blueprint_add_component). Mutating.
export const blueprintAddComponent: Tool = {
  name: "unreal_open_mcp_blueprint_add_component",
  description:
    "Add a component node to a Blueprint's Simple Construction Script. `path` " +
    "is the Blueprint asset object path (package-path form also accepted — " +
    "resolved in-memory first, then loaded). `component_class` is a " +
    "UActorComponent subclass path or short name (e.g. " +
    "'/Script/Engine.StaticMeshComponent' or 'StaticMeshComponent'). `name` is " +
    "the new component's variable name. Optional `parent_component` attaches " +
    "the new node under an existing SCS scene-component node; omitted → added " +
    "as a root node. Use blueprint_get first to learn valid existing " +
    "component names (the `parent` field on each component entry is the attach " +
    "parent — empty for a root). Mutating: runs the full gate path (checkpoint " +
    "-> add -> validate -> delta); `paths_hint` MUST list the Blueprint " +
    "package path (e.g. ['/Game/Mcp/BP_Thing']) — there is no whole-project " +
    "fallback, set gate:\"off\" to bypass. Result shape: { component, class }. " +
    "Error codes: missing_parameter (path/name/component_class absent), " +
    "blueprint_not_found (no Blueprint at path), no_scs (Blueprint has no " +
    "Simple Construction Script — not an Actor-based Blueprint), " +
    "invalid_component_class (class did not resolve or is not a " +
    "UActorComponent subclass), abstract_component (class is " +
    "abstract/deprecated — CreateNode would fatally assert, e.g. " +
    "LightComponentBase), invalid_name (name failed the Kismet name " +
    "validator), name_collision (name already used by a component, a member " +
    "variable, or a parent-class property), parent_not_found (parent_component " +
    "names no SCS node), invalid_attachment (the new class or the parent is " +
    "not a USceneComponent — attachment is a scene-graph op), create_failed " +
    "(CreateNode returned null), invalid_parameter (malformed body).",
  inputSchema: {
    type: "object",
    required: ["path", "component_class", "name", "paths_hint"],
    properties: {
      path: {
        type: "string",
        description:
          "Blueprint asset object path — an object path " +
          "('/Game/Mcp/BP_Thing.BP_Thing') or a package path " +
          "('/Game/Mcp/BP_Thing'). Resolved in-memory first (a Blueprint " +
          "created this session but not yet saved), then loaded.",
      },
      component_class: {
        type: "string",
        description:
          "UActorComponent subclass path or short name, e.g. " +
          "'/Script/Engine.StaticMeshComponent' or 'StaticMeshComponent'. " +
          "Must be concrete (not abstract/deprecated) — CreateNode would " +
          "fatally assert otherwise. The camelCase alias `componentClass` is " +
          "also accepted by the bridge.",
      },
      name: {
        type: "string",
        description:
          "Variable name for the new component (Kismet name validator rules). " +
          "Must not collide with an existing component, member variable, or " +
          "parent-class property — those namespaces share the generated " +
          "class's property namespace and would only fail at compile.",
      },
      parent_component: {
        type: "string",
        description:
          "Optional existing scene-component node to attach the new node " +
          "under (its SCS variable name). Both the new component and the " +
          "parent must be USceneComponents. Omit to add the node as a root. " +
          "The camelCase alias `parentComponent` is also accepted by the " +
          "bridge.",
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
