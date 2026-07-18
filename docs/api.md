# API and Protocol Surfaces

This file is the index for external interfaces and protocol contracts exposed by this repository.

## Domain reference

| Document | Covers | Status |
|---|---|---|
| `api/bridge-http.md` | Unreal bridge HTTP endpoints (`/ping`, `/tools/*`), envelopes, and errors. | `/ping` + `POST /tools/{name}` dispatch shipped; typed tools landing per phase |
| `api/mcp-tools.md` | MCP tool catalog, tool families, route policy (live/offline/local/batch), `capabilities` surface, rule + fix catalog contract. | `ping` + actor family (`actor_find`, `actor_create`, `actor_modify`, `object_modify`, `actor_set_parent`, `actor_duplicate`, `actor_destroy`, `actor_component_add`/`_destroy`/`_get`/`_modify`/`_list_all`) + level family (`level_open`, `level_save`, `level_list_loaded`, `level_set_current`, `level_unload_sublevel`, `level_get_data`, `level_create`) + gate meta-tools (`validate_edit`, `checkpoint_create`, `delta`) shipped; rest TBD |
| `api/resources.md` | MCP resource URIs, payload shapes, and resource router behavior. | TBD |

## Contract boundaries

- Bridge HTTP contract source: `packages/bridge/Source/UnrealOpenMcpEditor/Private/Bridge/` (`GET /ping` + `POST /tools/{name}` dispatch with the `{ok,result,error}` envelope shipped; mutating dispatches widen the envelope with a `gate` summary block per [Gate policy](api/bridge-http.md#gate-policy))
- Gate policy source: `packages/bridge/Source/UnrealOpenMcpEditor/Private/Gate/` (`UnrealOpenMcpGatePolicy` wraps every mutating dispatch in `checkpoint → mutate → validate → delta`; `UnrealOpenMcpVerifyGateAdapter` is the bridge→verify glue with the rule-selection + filter surface the meta-tools consult; `UnrealOpenMcpCheckpointStore` mirrors the gate's checkpoint for the meta-tools)
- Gate meta-tools source: `packages/bridge/Source/UnrealOpenMcpEditor/Private/MetaTools/UnrealOpenMcpGateMetaTools.h` (the three read-only tools `validate_edit` / `checkpoint_create` / `delta` — the explicit checkpoint → mutate → delta surface; bypass `GatePolicy.Execute` so they do not recurse)
- Instance lock + port resolver source: `packages/bridge/Source/UnrealOpenMcpRuntime/Public/Bridge/UnrealOpenMcpInstancePortResolver.h` (deterministic port + lock path), `packages/bridge/Source/UnrealOpenMcpEditor/Private/Bridge/UnrealOpenMcpBridgeInstanceLock.h` (lock file writer)
- SHA-256 source: `packages/bridge/Source/UnrealOpenMcpRuntime/Public/Crypto/UnrealOpenMcpSha256.h` (self-contained FIPS 180-4; byte-for-byte parity with Node `crypto.createHash('sha256')`)
- MCP server routing/registry source: `mcp-server/src/index.ts` (stdio server + tool dispatch), `mcp-server/src/live-client.ts` (live bridge HTTP client; routes `ping` → `GET /ping` and every other tool → `POST /tools/{name}`)
- MCP capabilities surface (local rule/fix catalog + builder): `mcp-server/src/capabilities/` (planned)
- MCP tool definitions source: `mcp-server/src/tools/` (`ping` + the actor family shipped: `actor_find`, `actor_create`, `actor_modify`, `object_modify`, `actor_set_parent`, `actor_duplicate`, `actor_destroy`, and the five `actor_component_*` tools; plus the level family shipped: `level_open`, `level_save`, `level_list_loaded`, `level_set_current`, `level_unload_sublevel`, `level_get_data`, `level_create`; plus the gate meta-tools shipped: `validate_edit`, `checkpoint_create`, `delta`; the rest land per phase)
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
