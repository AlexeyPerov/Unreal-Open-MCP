# API and Protocol Surfaces

This file is the index for external interfaces and protocol contracts exposed by this repository.

## Domain reference

| Document | Covers | Status |
|---|---|---|
| `api/bridge-http.md` | Unreal bridge HTTP endpoints (`/ping`, `/tools/*`), envelopes, and errors. | `/ping` shipped; `/tools/*` TBD |
| `api/mcp-tools.md` | MCP tool catalog, tool families, route policy (live/offline/local/batch), `capabilities` surface, rule + fix catalog contract. | TBD |
| `api/resources.md` | MCP resource URIs, payload shapes, and resource router behavior. | TBD |

## Contract boundaries

- Bridge HTTP contract source: `packages/bridge/Source/UnrealOpenMcpEditor/Private/Bridge/` (`GET /ping` shipped)
- Instance lock + port resolver source: `packages/bridge/Source/UnrealOpenMcpRuntime/Public/Bridge/UnrealOpenMcpInstancePortResolver.h` (deterministic port + lock path), `packages/bridge/Source/UnrealOpenMcpEditor/Private/Bridge/UnrealOpenMcpBridgeInstanceLock.h` (lock file writer)
- SHA-256 source: `packages/bridge/Source/UnrealOpenMcpRuntime/Public/Crypto/UnrealOpenMcpSha256.h` (self-contained FIPS 180-4; byte-for-byte parity with Node `crypto.createHash('sha256')`)
- MCP server routing/registry source: `mcp-server/src/index.ts` (stdio server + tool dispatch), `mcp-server/src/live-client.ts` (live bridge HTTP client; ping route shipped, `POST /tools/*` planned)
- MCP capabilities surface (local rule/fix catalog + builder): `mcp-server/src/capabilities/` (planned)
- MCP tool definitions source: `mcp-server/src/tools/` (`ping` shipped; the rest planned)
- MCP resources source: `mcp-server/src/resources/` (when shipped)

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
