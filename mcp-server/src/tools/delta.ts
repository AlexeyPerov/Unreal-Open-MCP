import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P3.6 — gate meta-tool: compare current project health vs a stored checkpoint.
// Returns `summary{newErrors,newWarnings,resolvedErrors,resolvedWarnings}`
// plus the newIssueKeys / resolvedIssueKeys lists. A missing checkpoint is
// NOT a tool failure — the store is session-scoped and a hot reload wipes it,
// so the response carries `unavailable:true` (+ `checkpointLostOnReload:true`
// when the store is completely empty) plus structured recovery guidance
// (agentNextSteps) instead of `isError:true`.
//
// Copied from Unity Open MCP's mcp-server/src/tools/delta.ts at copy fidelity.
// The unavailable / lost-on-reload payloads are shared contracts (agents that
// consume one consume the other) — the agentNextSteps guidance strings are
// identical.
//
// Route: live (POST /tools/unreal_open_mcp_delta). Read-only.
export const delta: Tool = {
  name: "unreal_open_mcp_delta",
  description:
    "Compare current project health vs a checkpoint created by " +
    "unreal_open_mcp_checkpoint_create. Returns summary counts " +
    "(newErrors/newWarnings/resolvedErrors/resolvedWarnings) plus the " +
    "newIssueKeys / resolvedIssueKeys lists. `passed` is true when no new " +
    "Errors were introduced. A missing checkpoint is NOT a tool failure: " +
    "checkpoints are session-scoped (in-memory) and a hot reload / editor " +
    "restart wipes them — the response carries `unavailable:true` (and " +
    "`checkpointLostOnReload:true` when the store is completely empty) plus " +
    "structured recovery guidance (agentNextSteps). Error codes: " +
    "missing_parameter (checkpoint_id absent), invalid_parameter (malformed body).",
  inputSchema: {
    type: "object",
    required: ["checkpoint_id"],
    properties: {
      checkpoint_id: {
        type: "string",
        description:
          "Checkpoint id returned by unreal_open_mcp_checkpoint_create (or by " +
          "a previous mutating dispatch's gate.checkpointId).",
      },
      paths: {
        type: "array",
        items: { type: "string" },
        description:
          "Re-validate scope; defaults to the checkpoint's stored paths. Pass " +
          "an explicit override when the mutation touched paths outside the " +
          "original hint.",
      },
    },
    additionalProperties: false,
  },
};
