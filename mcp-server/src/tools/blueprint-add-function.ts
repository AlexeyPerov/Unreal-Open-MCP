import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// Blueprint add function. Creates an empty user function-graph stub via
// FBlueprintEditorUtils::CreateNewGraph + AddFunctionGraph (the K2 schema
// auto-wires the entry/result nodes; AddFunctionGraph is called with a null
// SignatureFromObject so the MVP stub is parameter-less). `path` is the
// Blueprint asset object path (package-path form also accepted — resolved
// in-memory first, then loaded). `name` is the new function-graph name.
//
// STUB-ONLY SCOPE: this tool creates the callable function graph only. Body
// authoring (add_node / connect_pins / free-form wiring) is OUT OF SCOPE — the
// graph is empty until a later pack lands the node-authoring surface. The K2
// schema auto-wires the entry/result nodes, so the stub compiles cleanly.
//
// Guards (each maps to a structured error code):
//   - the name is validated by the Kismet name validator (invalid_name)
//   - the name must not collide with an existing function graph OR any UObject
//     outered to the Blueprint (name_collision). CreateNewGraph resolves an
//     outer-name clash by renaming the EXISTING object aside, so a name
//     colliding with the EventGraph (or any graph outered to the Blueprint)
//     would silently hijack it and report success — the pre-check turns that
//     into a structured error
//
// Mutating: runs the full gate path (checkpoint -> add -> validate -> delta);
// `paths_hint` MUST list the Blueprint package path (e.g.
// ['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set
// gate:"off" to bypass. Chain into blueprint_get to confirm the function now
// appears in the functions[] list, and chain into blueprint_compile to land
// it on the generated class.
//
// Fidelity: greenfield. No Unity Blueprint / prefab-graph twin. Behavior
// reference (read-only): Unreal-MCP's blueprint-add-function for the
// CreateNewGraph + AddFunctionGraph API + the outer-name hijack probe.
//
// Intentional deltas vs Unreal-MCP:
//   - Canonical MCP envelope + gate summary on add.
//   - Structured error codes (name_collision / create_graph_failed).
//   - Stub-only scope documented in the description.
//
// Route: live (POST /tools/unreal_open_mcp_blueprint_add_function). Mutating.
export const blueprintAddFunction: Tool = {
  name: "unreal_open_mcp_blueprint_add_function",
  description:
    "Add a user function-graph STUB to a Blueprint via " +
    "FBlueprintEditorUtils::CreateNewGraph + AddFunctionGraph (the K2 schema " +
    "auto-wires the entry/result nodes). `path` is the Blueprint asset object " +
    "path (package-path form also accepted — resolved in-memory first, then " +
    "loaded). `name` is the new function-graph name (Kismet name validator " +
    "rules). STUB-ONLY: body authoring (add_node / connect_pins / free-form " +
    "node wiring) is OUT OF SCOPE — the graph is empty; the entry/result " +
    "nodes are auto-wired by the K2 schema so it compiles. Mutating: runs " +
    "the full gate path (checkpoint -> add -> validate -> delta); " +
    "`paths_hint` MUST list the Blueprint package path (e.g. " +
    "['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set " +
    "gate:\"off\" to bypass. Result shape: { function }. Error codes: " +
    "missing_parameter (path/name absent), blueprint_not_found (no Blueprint " +
    "at path), invalid_name (name failed the Kismet name validator), " +
    "name_collision (name already used by a function graph OR any UObject " +
    "outered to the Blueprint — CreateNewGraph would otherwise rename the " +
    "existing object aside and silently hijack it), create_graph_failed " +
    "(CreateNewGraph returned null), invalid_parameter (malformed body).",
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
          "New function-graph name (Kismet name validator rules). Must not " +
          "collide with an existing function graph OR any UObject outered to " +
          "the Blueprint — CreateNewGraph would otherwise resolve the clash " +
          "by renaming the existing object aside and silently hijack it.",
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
