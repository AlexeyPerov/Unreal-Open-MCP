import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// Tool registry. P1.5 ships an empty array — the stdio server scaffold must
// boot, register handlers, and answer tools/list with an empty list before
// any real tool is wired in. The first tool (`unreal_open_mcp_ping`) lands in
// a later phase; each tool is defined in its own file under `src/tools/` and
// appended here.
//
// Every MCP tool follows the `unreal_open_mcp_*` naming convention. Tool
// definitions include: `name`, `description`, `inputSchema` (JSON Schema), and
// a handler attached at the routing layer.
export const ALL_TOOLS: Tool[] = [];
