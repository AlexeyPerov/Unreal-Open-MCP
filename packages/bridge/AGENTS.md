# Bridge package rules

## Scope

Rules for `packages/bridge/` — the Unreal Editor HTTP bridge (`Plugins/UnrealOpenMCP/`). Inherits root `AGENTS.md`; deeper rules win on overlap.

## Package shape

- Unreal Engine 5.6+ C++ Editor plugin. Editor code lives under `Source/UnrealOpenMcpEditor/`; runtime-safe infra under `Source/UnrealOpenMcpRuntime/` where needed for shared types.
- Hard dependency on the verify module — the gate flow calls into verify for checkpoint/validate/delta. Do not break this dependency direction.
- Editor-only tool handlers must not compile into packaged game builds unless explicitly opted in.
- Tests live under `Source/UnrealOpenMcpEditorTests/` (Automation specs).

## Tool registration

- Tools are registered with the HTTP server and dispatched via `POST /tools/{name}`.
- Every new tool must declare:
  - A unique `Name` (the MCP tool name, `unreal_open_mcp_*`).
  - Whether it is mutating (changes Unreal project/editor state).
  - Default gate mode for mutating tools (`Enforce` / `Warn` / `Off`).
  - Tool group id from the canonical catalog in `mcp-server/src/capabilities/tool-groups.ts`.
- Mutating tools must accept and honor the request-level `gate` value.
- When adding/removing/renaming a tool, update the MCP-side tool definition (`mcp-server/src/tools/`) in the same task.

## Tool-group visibility

- Sessions start with only the `core` group visible in `ListTools`; other groups are hidden until activated via `unreal_open_mcp_manage_tools`.
- The bridge does NOT track session state — the MCP server owns it. The bridge's role is compiled-state reporting only.
- `GET /tools` returns the tool inventory plus the group→tools map for capability probes.

## Gate policy

- The gate flow (checkpoint → mutate → validate → delta) is the bridge's core safety contract. Do not add a mutating dispatch path that bypasses `GatePolicy.Execute`.
- `paths_hint` is mandatory for mutating tool calls — there is no whole-project fallback. Do not add one.
- Gate precedence: request `gate` → project default → tool default.

## Transport

- Bridge HTTP server binds `127.0.0.1` by default. Remote bind is opt-in and requires auth when enabled.
- All UObject / editor API calls happen on the game thread via `GameThreadDispatcher`. Never call editor APIs from the HTTP listener worker thread.
- Paths use Unreal content paths (`/Game/...`, `/Engine/...`) and project-relative source paths under `Source/`.

## Auth

- A per-session bearer token is minted into the instance lock on bridge start and mirrored as `authToken` in the lock JSON. The TS-side `InstanceLock` interface must carry the same field.
- Enforcement is opt-in via `authMode` in project settings (`"none"` default | `"required"`).
- Token comparison must be constant-time.

## Multi-instance port + discovery

- The bridge port is **deterministic per project**: `20000 + (sha256(projectPath) % 10000)`, implemented in the bridge port resolver. Must match `mcp-server/src/instance-discovery.ts` byte-for-byte.
- `UNREAL_OPEN_MCP_BRIDGE_PORT` (env) overrides the deterministic default.
- Each running bridge writes a lock file at `~/.unreal-open-mcp/instances/<sha256(projectPath)>.json` via the instance lock module.
- Stale locks (crashed editor) are swept on `Acquire` by PID-liveness. The MCP server is read-only on the lock.
- Lock retention on hot reload: do not release the lock on module reload — stale heartbeat + live PID signals a dead bridge to the MCP server.

## UE version policy

- Develop and CI against **UE 5.8**. Support floor **UE 5.6+**.
- Do **not** pin `EngineVersion` in `UnrealOpenMCP.uplugin` — document the floor in docs/CI only (ADR-008).

## Verification

- C++ changes: add or update the narrowest Automation spec in `UnrealOpenMcpEditorTests/`.
- Tool contract changes: update the MCP-side tool definition in the same task.
- Gate flow changes: verify delta math in integration tests.
