import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// End-to-end health probe. The MCP server routes this to the bridge's
// `GET /ping` — one HTTP hop that verifies the whole chain (stdio client → MCP
// server → instance discovery → live bridge → game-thread dispatcher). It is
// the first tool many agents call to decide whether to proceed with live tools
// or fall back to offline reads.
//
// Copied from Unity Open MCP's mcp-server/src/tools/ping.ts (copy fidelity,
// P1.7), with the name changed to the `unreal_open_mcp_*` convention and a
// richer description so an agent reading tools/list understands the probe's
// role without consulting docs.
export const ping: Tool = {
  name: "unreal_open_mcp_ping",
  description:
    "Bridge health check. Verifies end-to-end connectivity from this MCP " +
    "server to the live Unreal bridge (stdio → instance discovery → HTTP " +
    "/ping). Returns the bridge's health payload (engine version, compile + " +
    "play state) on success, or a structured error distinguishing bridge-down " +
    "from timeout on failure. Call this first when live tools return " +
    "bridge_offline to confirm whether the editor is reachable.",
  inputSchema: {
    type: "object",
    properties: {},
    additionalProperties: false,
  },
};
