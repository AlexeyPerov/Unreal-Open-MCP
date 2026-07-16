import type { Tool } from "@modelcontextprotocol/sdk/types.js";
import { ping } from "./ping.js";
import { actorFind } from "./actor-find.js";
import { actorCreate } from "./actor-create.js";

// Tool registry. P1.7 registers the first real tool — `unreal_open_mcp_ping` —
// which the MCP server routes to the bridge's `GET /ping`. Each subsequent tool
// is defined in its own file under `src/tools/` and appended here.
//
// Every MCP tool follows the `unreal_open_mcp_*` naming convention. Tool
// definitions include: `name`, `description`, `inputSchema` (JSON Schema), and
// a handler attached at the routing layer (LiveClient → POST /tools/{name}).
//
// P2.2 adds the first real typed tool — `unreal_open_mcp_actor_find` (read-only
// actor locator). P2.3 adds the first mutating typed tool —
// `unreal_open_mcp_actor_create` (spawn in the current editor level; gate
// deferred). The rest of the actor family (modify / tree / components) lands in
// later P2 tasks and appends its tools here.
export const ALL_TOOLS: Tool[] = [ping, actorFind, actorCreate];
