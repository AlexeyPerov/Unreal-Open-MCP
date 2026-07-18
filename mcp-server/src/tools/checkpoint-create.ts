import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P3.6 — gate meta-tool: create a manual checkpoint for later delta comparison.
// Captures a fingerprint over `paths` (auto-selecting rules by extension when
// `categories` is omitted) and stores it in the bridge's session checkpoint
// store. Returns the checkpoint id + a per-rule {errors,warnings,issueKeys}
// summary the agent can use to assert the baseline shape before mutating.
//
// Copied from Unity Open MCP's mcp-server/src/tools/checkpoint-create.ts at
// copy fidelity. The only delta is the rule catalog (Unreal's v1 set is the
// three registered families). Storage contract: checkpoints are session-
// scoped (in-memory); a hot reload / editor restart wipes every checkpoint,
// so any checkpoint_id an agent holds is gone after a reload — delta surfaces
// this honestly as `checkpointLostOnReload`.
//
// Route: live (POST /tools/unreal_open_mcp_checkpoint_create). Read-only.
export const checkpointCreate: Tool = {
  name: "unreal_open_mcp_checkpoint_create",
  description:
    "Create a manual checkpoint for later delta comparison. Captures a verify " +
    "fingerprint over `paths` (auto-selecting rules by extension when " +
    "`categories` is omitted) and stores it in the bridge's session checkpoint " +
    "store. Returns the checkpoint id + a per-rule {errors,warnings,issueKeys} " +
    "summary. Checkpoints are session-scoped: a hot reload or editor restart " +
    "wipes them, and a subsequent unreal_open_mcp_delta call surfaces this " +
    "honestly as `unavailable:true` / `checkpointLostOnReload:true` rather " +
    "than a hard error. Pair with unreal_open_mcp_delta for a manual " +
    "checkpoint → mutate → delta workflow.",
  inputSchema: {
    type: "object",
    properties: {
      paths: {
        type: "array",
        items: { type: "string" },
        description:
          "Scope; empty = whole project summary (expensive — every registered " +
          "rule runs over an empty scope). Prefer at least one content path.",
      },
      categories: {
        type: "array",
        items: { type: "string" },
        description:
          "Verify rule IDs to pin (overrides auto-select by extension). Empty " +
          "→ auto-select from paths.",
      },
      label: {
        type: "string",
        description: "Optional agent-supplied label surfaced in the store UI.",
      },
    },
    additionalProperties: false,
  },
};
