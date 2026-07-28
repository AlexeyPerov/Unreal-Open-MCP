// Shared constants for the Unreal Open MCP CLI.
//
// Pinned here (not inlined) so the arg parser and the help text agree on the
// exact env-var names, and so a future command module can reuse them. The names
// mirror the MCP server / instance-discovery contract (P1.4, P1.6) and the
// Phase 8 shared invariants.

/** Project root (absolute path to the `.uproject` parent). */
export const PROJECT_PATH_ENV_VAR = "UNREAL_PROJECT_PATH";

/** Optional bridge port override (same precedence as the MCP server). */
export const PORT_ENV_VAR = "UNREAL_OPEN_MCP_BRIDGE_PORT";

/** Default invocation name shown in help text. */
export const BIN_NAME = "unreal-open-mcp-cli";
