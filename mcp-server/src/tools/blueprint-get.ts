import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// Blueprint get — read-only scoped graph summary for LLM inspection. Returns
// the Blueprint's identity (name / path / parentClass) plus the structural
// surface an agent needs to plan edits: member variables (name/type/array +
// optional default), SCS components (name/class/parent — the attach parent
// is empty for a root node, and drives P6.4's add-component parentComponent),
// user function graphs with node-counts, events with an `enabled` flag
// (fresh Actor Blueprints are pre-seeded with DISABLED ghost event nodes;
// `enabled` mirrors the add-event semantics so an agent does not conclude a
// disabled ghost already fires), implemented interfaces, and the parent
// class chain.
//
// `path` accepts an object path ('/Game/Mcp/BP_Thing.BP_Thing') or a package
// path ('/Game/Mcp/BP_Thing'); the bridge resolves in-memory first (assets
// created earlier this session may be unsaved), then loads.
//
// Fidelity: greenfield. No Unity twin. Behavior reference (read-only):
// Unreal-MCP's blueprint-get for the summary DTO shape + the disabled-ghost-
// event `enabled` flag.
//
// Route: live (POST /tools/unreal_open_mcp_blueprint_get). Read-only — no
// gate, no paths_hint.
export const blueprintGet: Tool = {
  name: "unreal_open_mcp_blueprint_get",
  description:
    "Read a scoped graph summary of a Blueprint for inspection. Read-only " +
    "(gate-free). Returns { name, path, parentClass, variables, components, " +
    "functions, events, interfaces, parentChain } where: `variables` is " +
    "[{ name, type, isArray, default? }]; `components` is " +
    "[{ name, class, parent }] (parent is the SCS attach parent, empty for a " +
    "root node — drives add-component's parentComponent); `functions` is " +
    "[{ name, nodeCount }]; `events` is [{ name, enabled }] — a fresh Actor " +
    "Blueprint is pre-seeded with DISABLED ghost event nodes " +
    "(ReceiveBeginPlay/ReceiveTick), so `enabled:false` means the event does " +
    "NOT fire until an add-event enables it; `interfaces` is " +
    "[classPath]; `parentChain` is [className] up to UObject. `path` accepts " +
    "an object path ('/Game/Mcp/BP_Thing.BP_Thing') or a package path " +
    "('/Game/Mcp/BP_Thing') — resolved in-memory first (unsaved session " +
    "assets), then loaded. Use after blueprint_create to confirm the summary, " +
    "and before structure edits to learn valid variable/component/function " +
    "names. Error codes: missing_parameter (path absent), blueprint_not_found " +
    "(no Blueprint at path), invalid_parameter (malformed body).",
  inputSchema: {
    type: "object",
    required: ["path"],
    properties: {
      path: {
        type: "string",
        description:
          "Blueprint asset object path — an object path " +
          "('/Game/Mcp/BP_Thing.BP_Thing') or a package path " +
          "('/Game/Mcp/BP_Thing'). Resolved in-memory first (a Blueprint " +
          "created this session but not yet saved), then loaded from disk.",
      },
    },
    additionalProperties: false,
  },
};
