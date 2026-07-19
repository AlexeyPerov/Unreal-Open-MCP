import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P5.3 — empty the bridge's log ring buffer. The Unreal analog of Unity's
// console-clear.
//
// Classified read-only: the ring is a plugin-owned diagnostic buffer, not
// project/editor state, so clearing it is a buffer-local meta op that
// dispatches directly without the gate (mirrors Unity/Godot treating a console
// clear as non-mutating).
//
// Route: live (POST /tools/unreal_open_mcp_console_clear_logs). Read-only.
export const consoleClearLogs: Tool = {
  name: "unreal_open_mcp_console_clear_logs",
  description:
    "Empty the engine log ring buffer, returning { removed } (the number of " +
    "entries dropped). Read-only classification: the ring is a plugin-owned " +
    "diagnostic buffer, not project/editor state, so this is not a gated " +
    "mutation. Sequence numbers stay monotonic across a clear, so entries " +
    "captured after a clear are still distinguishable from earlier ones. Error " +
    "code: invalid_parameter (malformed body).",
  inputSchema: {
    type: "object",
    properties: {},
    additionalProperties: false,
  },
};
