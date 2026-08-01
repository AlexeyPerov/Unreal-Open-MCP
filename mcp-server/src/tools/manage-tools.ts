import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// manage_tools — per-session tool-group visibility mutator (P8.10).
//
// Server-only meta-tool: the MCP server holds the per-session group state
// (ToolSessionState); the bridge does NOT track session visibility and never
// sees these calls. Activating a group makes its tools appear in subsequent
// ListTools responses; deactivating removes them. reset() restores the
// catalog's default-on groups (the `core` baseline).
//
// The tool definition ships with every build and is ALWAYS visible — it is in
// the ALWAYS_VISIBLE_TOOLS allow-list in tool-session-state.ts and carries no
// group assignment (groupFor → null), so an agent can always reach it before
// any other group is active (the one mutator you need to re-light a torn-down
// session). It routes local — resolved in-process by the ToolRouter, never
// POSTed to the bridge.
//
// When an activate / deactivate / reset actually changes the visible tool set,
// the server emits `notifications/tools/list_changed` so a compliant client
// refreshes its tools/list cache. No-op actions (activating an already-active
// group, etc.) do NOT emit.
//
// Copied from Unity Open MCP's mcp-server/src/tools/manage-tools.ts (copy
// fidelity) with these intentional deltas:
//   1. Smaller action set — `suggest` / `activate_for` (Unity's intent→group
//      recommendation engine) are deferred; the v1 surface is the four core
//      actions. A name changed to the `unreal_open_mcp_*` prefix (ADR-003).
//   2. Default-on set follows the Unreal lean baseline (`core` only); Unity
//      also defaults `gate-and-verify`.
//   3. No domain-package auto-activation (Unreal domain packs are P12) — so the
//      list_groups payload has no `available` / `autoActivated` fields.
//
// Route: local (handler in src/tool-router.ts). Read-only for list_groups;
// session-mutating for activate / deactivate / reset (NOT a gate mutator — no
// paths_hint, no editor/project state touched).
export const manageTools: Tool = {
  name: "unreal_open_mcp_manage_tools",
  description:
    "Manage which tool groups are visible in this session. A fresh session " +
      "activates only the default-on groups (`core`); activate other groups on " +
      "demand so their tools appear in tools/list. Actions: " +
      "`list_groups` (enumerate every group with its active flag, default-on " +
      "flag, activation source, and tool roster), `activate` / `deactivate` " +
      "(toggle one group — requires `group`), `reset` (restore the default-on " +
      "set). When an activate / deactivate / reset changes the visible surface " +
      "the server emits `notifications/tools/list_changed` so a compliant " +
      "client refreshes its tool list. Activation is ephemeral and per-session " +
      "(in-memory; resets on server restart). " +
      "Route: local — the server resolves this in-process; the live bridge is " +
      "NOT required and never sees these calls. " +
      "Error codes: `missing_parameter` (no `action`, or `activate`/`deactivate` " +
      "without `group`), `unknown_action`, `unknown_group`.",
  inputSchema: {
    type: "object",
    properties: {
      action: {
        enum: ["list_groups", "activate", "deactivate", "reset"],
        description:
          "list_groups: enumerate every group. activate / deactivate: toggle " +
            "one group (requires `group`). reset: restore the default-on set.",
      },
      group: {
        type: "string",
        description:
          "Group id (required for activate / deactivate). Valid ids are " +
            "returned by list_groups. Unknown ids return `unknown_group`.",
      },
    },
    required: ["action"],
    additionalProperties: false,
  },
};
