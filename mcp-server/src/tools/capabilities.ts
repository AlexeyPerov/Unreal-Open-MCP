import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P3.8 — capabilities: discover the full capability surface in one call.
//
// Local-route (no bridge round-trip). The MCP server resolves this tool in-
// process: it builds the capabilities payload from the registered tool list
// and the static rule/fix catalog (mcp-server/src/capabilities/), and returns
// it directly — no POST /tools/{name}. An agent can call this with the editor
// down and still get an accurate rule/fix/tool inventory.
//
// Copied from Unity Open MCP's mcp-server/src/tools/agent-capabilities.ts
// (copy fidelity) with the name switched to the `unreal_open_mcp_*` prefix.
// The schema + description are shared contracts — agents that consume one
// consume the other. The only delta is the catalog contents (Unreal's v1
// rule/fix surface: broken_soft_references / missing_blueprint_parents /
// compile_errors + the Safe fix clear_broken_soft_reference).
//
// Route: local (handler in src/index.ts). Read-only.
export const capabilities: Tool = {
  name: "unreal_open_mcp_capabilities",
  description:
    "Discover the full capability surface in one call: every tool with its input schema and route policy, " +
    "every verify rule with applicable asset kinds and issue severities, and every available fix. " +
    "Each capability carries an `implemented` boolean; planned-but-unbuilt items return with " +
    "`status: \"planned\"` and actionable guidance instead of failing. Call this first to learn what is " +
    "available before using actor / level / gate tools blindly. " +
    "Route: local — the server resolves this in-process; the live bridge is NOT required. " +
    "Implemented verify rules: broken_soft_references (issue: broken_soft_reference → fix: " +
    "clear_broken_soft_reference, Safe), missing_blueprint_parents (issue: missing_blueprint_parent, " +
    "no Safe fix in v1), compile_errors (issue: compile_error, no Safe fix in v1).",
  inputSchema: {
    type: "object",
    properties: {
      kind: {
        enum: ["tools", "rules", "fixes"],
        description:
          "Filter to a single surface. Omit to return tools + rules + fixes together.",
      },
      include_planned: {
        type: "boolean",
        default: true,
        description:
          "Include planned-but-unbuilt capabilities (status \"planned\"). Set false to see only implemented items.",
      },
    },
    additionalProperties: false,
  },
};
