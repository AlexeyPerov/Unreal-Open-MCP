import type { Tool } from "@modelcontextprotocol/sdk/types.js";
import { ping } from "./ping.js";

// Tool registry. P1.7 registers the first real tool — `unreal_open_mcp_ping` —
// which the MCP server routes to the bridge's `GET /ping`. Each subsequent tool
// is defined in its own file under `src/tools/` and appended here.
//
// Every MCP tool follows the `unreal_open_mcp_*` naming convention. Tool
// definitions include: `name`, `description`, `inputSchema` (JSON Schema), and
// a handler attached at the routing layer.
export const ALL_TOOLS: Tool[] = [ping];
