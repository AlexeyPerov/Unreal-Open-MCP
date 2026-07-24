import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// Blueprint remove component. Deletes an SCS node by variable name via
// RemoveNodeAndPromoteChildren — the node is removed and any child
// scene-component nodes are re-parented onto the removed node's parent, so a
// subtree is never orphaned. MarkBlueprintAsStructurallyModified follows so a
// later compile rebuilds the CDO without the component.
//
// `path` is the Blueprint asset object path (package-path form also accepted —
// resolved in-memory first, then loaded). `name` is the variable name of the
// SCS node to remove (use blueprint_get to learn the names — the `name` field
// on each component entry). Operates on the Blueprint asset's SCS, not on a
// live actor instance's components (those are the actor_component_* tools).
//
// Mutating: runs the full gate path (checkpoint -> remove -> validate ->
// delta); `paths_hint` MUST list the Blueprint package path (e.g.
// ['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set
// gate:"off" to bypass. Chain into blueprint_get to confirm the component is
// gone (and any children survived, promoted).
//
// Fidelity: greenfield. No Unity Blueprint / prefab-graph twin. Behavior
// reference (read-only): Unreal-MCP's blueprint-remove-component for the SCS
// remove + promote-children contract.
//
// Intentional deltas vs Unreal-MCP:
//   - Canonical MCP envelope + gate summary on remove.
//
// Route: live (POST /tools/unreal_open_mcp_blueprint_remove_component).
// Mutating.
export const blueprintRemoveComponent: Tool = {
  name: "unreal_open_mcp_blueprint_remove_component",
  description:
    "Remove a component node from a Blueprint's Simple Construction Script by " +
    "variable name. `path` is the Blueprint asset object path (package-path " +
    "form also accepted — resolved in-memory first, then loaded). `name` is " +
    "the variable name of the SCS node to remove (use blueprint_get to learn " +
    "the names — the `name` field on each component entry). Children are " +
    "promoted: child scene-component nodes are re-parented onto the removed " +
    "node's parent, so a subtree is never orphaned. Operates on the Blueprint " +
    "asset SCS, not on a live actor instance (use actor_component_destroy for " +
    "instance components). Mutating: runs the full gate path (checkpoint -> " +
    "remove -> validate -> delta); `paths_hint` MUST list the Blueprint " +
    "package path (e.g. ['/Game/Mcp/BP_Thing']) — there is no whole-project " +
    "fallback, set gate:\"off\" to bypass. Result shape: {} (empty success — " +
    "chain into blueprint_get to confirm the component is gone). Error codes: " +
    "missing_parameter (path/name absent), blueprint_not_found (no Blueprint " +
    "at path), no_scs (Blueprint has no Simple Construction Script), " +
    "component_not_found (no SCS node with that variable name), " +
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
          "Variable name of the SCS component node to remove (the `name` " +
          "field on a component entry returned by blueprint_get). Children " +
          "are promoted onto the removed node's parent.",
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
          "Gate mode — enforce (default) runs checkpoint -> remove -> " +
          "validate -> delta and hard-fails on new Errors; warn commits the " +
          "mutation but surfaces new Errors as warnings; off skips the gate " +
          "entirely (paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};
