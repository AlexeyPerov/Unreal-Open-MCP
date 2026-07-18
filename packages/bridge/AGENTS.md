# Bridge package rules

## Scope

Rules for `packages/bridge/` — the Unreal Editor HTTP bridge (`Plugins/UnrealOpenMCP/`). Inherits root `AGENTS.md`; deeper rules win on overlap.

## Package shape

- Unreal Engine 5.6+ C++ Editor plugin. Editor code lives under `Source/UnrealOpenMcpEditor/`; runtime-safe infra under `Source/UnrealOpenMcpRuntime/` where needed for shared types.
- Hard dependency on the verify module — the gate flow calls into verify for checkpoint/validate/delta. Do not break this dependency direction.
- Editor-only tool handlers must not compile into packaged game builds unless explicitly opted in.
- Tests live under `Source/UnrealOpenMcpEditorTests/` (Automation specs).

## Editor / Runtime boundary

The load-bearing invariant is one-directional: **Editor code may reference Runtime code; Runtime code may NEVER reference Editor code.** Runtime must stay free of `UnrealEd.h`/`Editor.h` umbrella includes, editor module include roots (`"UnrealEd/"`, `"AssetTools/"`, `"Editor/"`, `"Subsystems/EditorSubsystem"`...), and editor-only `Build.cs` dependencies (`UnrealEd`, `Slate`, `AssetTools`, ...). ModuleRules enforce linking; the include/surface leak is enforced by `scripts/check-editor-boundary.py`, which runs as a **blocking CI guard** (the `editor-boundary` job) — it is advisory to ModuleRules, not a replacement. Both must hold.

Run locally:

```
python scripts/check-editor-boundary.py            # scan the Runtime module
python scripts/check-editor-boundary.py --self-test  # verify the detector itself
```

The deny-list of editor headers/modules lives in the script so the policy is readable in the PR that changes it. Start strict; narrow only on a justified false positive. To suppress a genuine, reviewed exception, add a comment carrying the marker **with a justification** on the offending line or the line above it — a bare marker is itself a violation:

```cpp
#include "UnrealEd.h"  // unreal-open-mcp:allow-editor-boundary: <why this is safe>
```

## Tool registration

- Tools are registered with the HTTP server and dispatched via `POST /tools/{name}`.
- Every new tool must declare:
  - A unique `Name` (the MCP tool name, `unreal_open_mcp_*`).
  - Whether it is mutating (changes Unreal project/editor state) via the registry metadata API.
  - Default gate mode for mutating tools (`Enforce` / `Warn` / `Off`) — the metadata consulted when neither the request nor the project default applies.
  - Tool group id from the canonical catalog in `mcp-server/src/capabilities/tool-groups.ts`.
- Registry API:
  - `Register(name, handler)` — read-only shorthand. Defaults to non-mutating with gate Off (no gate path).
  - `Register(name, handler, FUnrealOpenMcpToolMetadata::Mutating())` — mutating tool. Defaults to gate Enforce; the dispatch policy routes it through `FUnrealOpenMcpGatePolicy::Execute`.
- Mutating tools must accept `paths_hint: string[]` (mandatory, no whole-project fallback) and the optional request-level `gate` value.
- When adding/removing/renaming a tool, update the MCP-side tool definition (`mcp-server/src/tools/`) in the same task.

## Tool-group visibility

- Sessions start with only the `core` group visible in `ListTools`; other groups are hidden until activated via `unreal_open_mcp_manage_tools`.
- The bridge does NOT track session state — the MCP server owns it. The bridge's role is compiled-state reporting only.
- `GET /tools` returns the tool inventory plus the group→tools map for capability probes.

## Gate policy

- The gate flow (checkpoint → mutate → validate → delta) is the bridge's core safety contract. Do not add a mutating dispatch path that bypasses `FUnrealOpenMcpGatePolicy::Execute` (the single dispatch chokepoint wired in `HandleToolDispatch`).
- `paths_hint` is mandatory for mutating tool calls — there is no whole-project fallback. Do not add one. An empty hint fails fast with `paths_hint_required` BEFORE any mutation runs; only `gate:"off"` bypasses the hint.
- Gate precedence: request `gate` → tool default (the project default slot is wired when the settings tab lands). Request `gate` is case-sensitive; unknown values fall back to Enforce.
- Mutating tools register with `FUnrealOpenMcpToolMetadata::Mutating()` so the dispatch policy knows to route them through the gate. Read-only tools use the `Register(name, handler)` shorthand (defaults to non-mutating + gate Off).
- The widened envelope (P3.5) adds a `gate` summary block to mutating dispatches. `ok` and `error.code` stay stable across the P2.1 → P3.5 widening; only additive fields appear. See [docs/api/bridge-http.md#gate-policy](../../docs/api/bridge-http.md#gate-policy).
- Gate outcomes are stable wire tokens: `passed` / `warned` / `failed` / `skipped` / `validate_scan_failed`. `validate_scan_failed` is set ONLY by the validate-exception catch in `Execute` — `ResolveOutcome` must never produce it (parity invariant pinned by `UnrealOpenMcpGatePolicySpec`).
- `unreal_open_mcp_apply_fix` (P3.7) is the one tool that bypasses the generic gate path in `HandleToolDispatch`. Dry-run (`dry_run:true`, the default) dispatches the inner handler directly (no gate around a preview that mutates nothing); non-dry-run applies route through `FUnrealOpenMcpApplyFixGateRunner`, which wraps `FUnrealOpenMcpGatePolicy::Execute` with a `FUnrealOpenMcpFixRollback` snapshot so a corrupting fix is auto-reverted on failure or new errors under `enforce`. The inner handler refuses a non-dry-run apply without a rollback snapshot (`rollback_unavailable`) so a batch-execute path or a direct dispatch cannot bypass the protection. `paths_hint` is auto-derived from the issue's asset path when the caller omits it (Unity parity).

## Transport

- Bridge HTTP server binds `127.0.0.1` by default. Remote bind is opt-in and requires auth when enabled.
- All UObject / editor API calls happen on the game thread via `GameThreadDispatcher`. Never call editor APIs from the HTTP listener worker thread.
- Paths use Unreal content paths (`/Game/...`, `/Engine/...`) and project-relative source paths under `Source/`.

## Auth

- A per-session bearer token is minted into the instance lock on bridge start and mirrored as `authToken` in the lock JSON. The TS-side `InstanceLock` interface must carry the same field.
- **P1.4 deferral:** `authToken` is NOT yet minted — the field is omitted from the lock JSON and its absence is pinned in `UnrealOpenMcpInstanceLockSpec`. When auth enforcement lands (later phase), add the field between `port` and `projectPath` (Unity's order) and update the TS reader + spec in the same task.
- Enforcement is opt-in via `authMode` in project settings (`"none"` default | `"required"`).
- Token comparison must be constant-time.

## Multi-instance port + discovery

- The bridge port is **deterministic per project**: `20000 + (sha256(projectPath) % 10000)`, implemented in `FUnrealOpenMcpInstancePortResolver` (Runtime). Must match `mcp-server/src/instance-discovery.ts` byte-for-byte — cross-side consistency is pinned by golden vectors in `UnrealOpenMcpPortResolverSpec` and the TS `instance-discovery.test.ts` (P1.6). If either side changes, update both in the same task.
- **SHA-256 source:** `FUnrealOpenMcpSha256` (Runtime, self-contained FIPS 180-4). `FSHA1` is SHA-1 and MUST NOT be used — using it would silently break parity with Node `crypto.createHash('sha256')`.
- Port resolution precedence: `UNREAL_OPEN_MCP_BRIDGE_PORT` (env) → `-UNREAL_OPEN_MCP_BRIDGE_PORT=<n>` (CLI) → deterministic hash. Env wins over CLI.
- Each running bridge writes a lock file at `~/.unreal-open-mcp/instances/<sha256(projectPath)>.json` via `FUnrealOpenMcpBridgeInstanceLock` (Editor). The resolver (formula + path) lives in Runtime so a future packaged commandlet can derive its port; the lock file writer lives in Editor because it owns the heartbeat lifecycle.
- Stale locks (crashed editor) are swept on `Acquire` by PID-liveness (`FPlatformProcess::GetProcessIsAlive`). The MCP server is read-only on the lock.
- Lock retention on hot reload: do not release the lock on module reload — stale heartbeat + live PID signals a dead bridge to the MCP server.

## UE version policy

- Develop and CI against **UE 5.8**. Support floor **UE 5.6+**.
- Do **not** pin `EngineVersion` in `UnrealOpenMCP.uplugin` — document the floor in docs/CI only (ADR-008).

## Verification

- C++ changes: add or update the narrowest Automation spec in `UnrealOpenMcpEditorTests/`.
- Tool contract changes: update the MCP-side tool definition in the same task.
- Gate flow changes: verify delta math in integration tests.
