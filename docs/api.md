# API and Protocol Surfaces

This file is the index for external interfaces and protocol contracts exposed by this repository.

## Domain reference

| Document | Covers | Status |
|---|---|---|
| `api/bridge-http.md` | Unreal bridge HTTP endpoints (`/ping`, `/tools/*`), envelopes, and errors. | `/ping` + `POST /tools/{name}` dispatch shipped; typed tools landing per phase |
| `api/mcp-tools.md` | MCP tool catalog, tool families, route policy (live/offline/local/batch), `capabilities` surface, rule + fix catalog contract. | `ping` + actor family (`actor_find`, `actor_create`, `actor_modify`, `object_modify`, `actor_set_parent`, `actor_duplicate`, `actor_destroy`, `actor_component_add`/`_destroy`/`_get`/`_modify`/`_list_all`) + level family (`level_open`, `level_save`, `level_list_loaded`, `level_set_current`, `level_unload_sublevel`, `level_get_data`, `level_create`) + gate meta-tools (`validate_edit`, `checkpoint_create`, `delta`) + `apply_fix` + `capabilities` (local-route; discovers tools + verify rules + fixes in one call) + asset read family (`asset_find`, `asset_get_data`) shipped; rest TBD |
| `api/resources.md` | MCP resource URIs, payload shapes, and resource router behavior. | TBD |

## Contract boundaries

- Bridge HTTP contract source: `packages/bridge/Source/UnrealOpenMcpEditor/Private/Bridge/` (`GET /ping` + `POST /tools/{name}` dispatch with the `{ok,result,error}` envelope shipped; mutating dispatches widen the envelope with a `gate` summary block per [Gate policy](api/bridge-http.md#gate-policy))
- Gate policy source: `packages/bridge/Source/UnrealOpenMcpEditor/Private/Gate/` (`UnrealOpenMcpGatePolicy` wraps every mutating dispatch in `checkpoint → mutate → validate → delta`; `UnrealOpenMcpVerifyGateAdapter` is the bridge→verify glue with the rule-selection + filter surface the meta-tools consult; `UnrealOpenMcpCheckpointStore` mirrors the gate's checkpoint for the meta-tools)
- Gate meta-tools source: `packages/bridge/Source/UnrealOpenMcpEditor/Private/MetaTools/UnrealOpenMcpGateMetaTools.h` (the three read-only tools `validate_edit` / `checkpoint_create` / `delta` — the explicit checkpoint → mutate → delta surface; bypass `GatePolicy.Execute` so they do not recurse)
- apply_fix source: `packages/bridge/Source/UnrealOpenMcpEditor/Private/MetaTools/UnrealOpenMcpApplyFixTool.h` (the fix-application workflow — dry-run preview lists the description / safe flag; non-dry-run applies run through the gate runner with a `FixRollback` snapshot so a corrupting fix is auto-reverted on failure or new errors under `enforce`; v1 ships the single Safe provider `clear_broken_soft_reference`)
- Instance lock + port resolver source: `packages/bridge/Source/UnrealOpenMcpRuntime/Public/Bridge/UnrealOpenMcpInstancePortResolver.h` (deterministic port + lock path), `packages/bridge/Source/UnrealOpenMcpEditor/Private/Bridge/UnrealOpenMcpBridgeInstanceLock.h` (lock file writer)
- SHA-256 source: `packages/bridge/Source/UnrealOpenMcpRuntime/Public/Crypto/UnrealOpenMcpSha256.h` (self-contained FIPS 180-4; byte-for-byte parity with Node `crypto.createHash('sha256')`)
- MCP server routing/registry source: `mcp-server/src/index.ts` (stdio server + tool dispatch), `mcp-server/src/live-client.ts` (live bridge HTTP client; routes `ping` → `GET /ping` and every other tool → `POST /tools/{name}`)
- MCP capabilities surface (local rule/fix catalog + builder): `mcp-server/src/capabilities/` (shipped — `rule-catalog.ts` mirrors the implemented verify rules and fix providers; `build-capabilities.ts` assembles the capabilities payload; `unreal_open_mcp_capabilities` is local-route, no bridge hop)
- MCP tool definitions source: `mcp-server/src/tools/` (`ping` + the actor family shipped: `actor_find`, `actor_create`, `actor_modify`, `object_modify`, `actor_set_parent`, `actor_duplicate`, `actor_destroy`, and the five `actor_component_*` tools; plus the level family shipped: `level_open`, `level_save`, `level_list_loaded`, `level_set_current`, `level_unload_sublevel`, `level_get_data`, `level_create`; plus the gate meta-tools shipped: `validate_edit`, `checkpoint_create`, `delta`; plus `apply_fix`; plus `capabilities` (local-route discovery); plus the asset read family shipped: `asset_find`, `asset_get_data`; the rest land per phase)
- MCP resources source: `mcp-server/src/resources/` (when shipped)
- Phase 1 parity smoke sources: `mcp-server/src/integration.test.ts` (in-process MCP client↔server over in-memory transport), `mcp-server/scripts/p1-parity-smoke.mjs` (scripted stdio smoke against the built `dist/index.js`)
- Phase 2 parity smoke sources: `mcp-server/src/integration.test.ts` (P2.8 cases — `unreal_open_mcp_actor_find` round-trip over `POST /tools/{name}` with the `{ok,result,error}` envelope), `mcp-server/scripts/p2-parity-smoke.mjs` (scripted stdio smoke against the built `dist/index.js`; healthy / bridge-down / tool-error cases)

## Contract documentation guidance

- Prefer documenting behavior and payload shapes over implementation details.
- Call out breaking changes explicitly.
- Keep examples minimal and representative.
- Unreal content paths use `/Game/...`; document live/offline/local/batch routes per tool.

## Update triggers

Update this index when:

- a new API/protocol doc is added,
- contract ownership moves to new modules,
- endpoint or resource domains are reorganized.
