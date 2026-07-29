# MCP server rules

## Scope

Rules for `mcp-server/` — the stdio MCP server (`unreal-open-mcp`). Inherits root `AGENTS.md`; deeper rules win on overlap.

## Package shape

- TypeScript ESM project (`"type": "module"`). Source under `src/`, built to `dist/`.
- Node 18+. Type-stripping (`--experimental-strip-types`) may be used for tests; keep imports statically resolvable — no dynamic `import()` for internal modules.
- No runtime dependencies beyond `@modelcontextprotocol/sdk`. Do not add dependencies without strong justification.

## Tool definitions

- Every MCP tool is defined in `src/tools/{tool-name}.ts` and exported from `src/tools/index.ts`.
- Tool names follow the `unreal_open_mcp_*` convention. The prefix signals routing to agents.
- Every tool definition includes: `name`, `description`, `inputSchema` (JSON Schema), and a handler.
- When a tool's schema changes, the bridge-side C++ handler must stay in sync in the same task — the bridge parses args by key name, not by schema validation.

## Routing

- `src/tool-router.ts` is the single dispatch spine for every `tools/call` — `handleCallTool` in `index.ts` delegates to it. It classifies a tool name via `routePolicy(name)` into one of four routes and stamps `_source` + `_route: { route }` metadata on every JSON result (success AND error) so agents can branch on where a response came from. Route policies:
  - **live** (default) — POSTs to the bridge via the injected `LiveClient` (`unreal_open_mcp_ping` → `GET /ping`; everything else → `POST /tools/{name}`). Stamped `_source: "live"`, `_route.route: "live"`.
  - **offline** — local disk parsers, no editor needed. Empty today; lands in P8.7. A tool classified offline before its handler lands returns `offline_not_implemented` (never a silent live fallthrough).
  - **local** — resolved in-process, never hits the bridge. Wired today: `unreal_open_mcp_capabilities`, `unreal_open_mcp_bridge_status`. Forward-looking local-only tools (`generate_skill`, `manage_tools`) must never depend on the live bridge when they land. Stamped `_source: "local"`, `_route.route: "local"`.
  - **batch** — headless Unreal commandlet (narrower than Unity batch). Empty today; a tool classified batch returns `batch_not_implemented` until the spawn lands.
- The policy table lives in `routePolicy` (module-level sets: `LOCAL_ROUTE_TOOLS` / `OFFLINE_ROUTE_TOOLS` / `BATCH_ROUTE_TOOLS`; default `live`). Adding a routed tool means extending the matching set AND its handler in `ToolRouter`.
- Do not add a new route type without updating `docs/architecture.md` and the route-policy table in `docs/api/mcp-tools.md`.
- `unreal_open_mcp_capabilities` and `unreal_open_mcp_bridge_status` are **local-only** today — they must never depend on the live bridge (bridge_status fires one `/ping` probe through the live transport but resolves the status in-process).

## Tool-group visibility

- Canonical group catalog: `src/capabilities/tool-groups.ts`.
- Per-session state: `src/tool-session-state.ts` (`ToolSessionState`). Ephemeral, in-memory, per stdio server process. Resets to `core`-only on restart.
- `ListTools` filters via `filterVisibleTools(ALL_TOOLS, sessionState)` in `index.ts`. Always-visible meta-tools bypass the filter via the `ALWAYS_VISIBLE_TOOLS` allow-list.
- `manage_tools` mutates session state from `tool-router.ts`. The bridge does NOT see these calls.
- When activate/deactivate/reset changes the visible tool set, emit `notifications/tools/list_changed`.

## Instance discovery

- Bridge port resolution lives in `src/instance-discovery.ts`. Precedence: `UNREAL_OPEN_MCP_BRIDGE_PORT` env var → `~/.unreal-open-mcp/instances/<sha256(projectPath)>.json` lock file (when its `pid` is alive) → deterministic hash `20000 + (sha256(path) % 10000)`.
- The hash formula must stay byte-for-byte identical to the bridge mirror in `packages/bridge/`. Cross-side consistency is pinned by tests — if either side changes, update both in the same task.
- The MCP server is **read-only** on the lock file. Stale-lock cleanup is the bridge's job on its next `Acquire`; never delete or rewrite locks from this package.
- No new runtime deps: the module uses only `node:crypto`, `node:fs`, `node:os`, `node:path`.
- The lock file carries an `authToken`. `resolveAuthToken` reads it and threads it into `LiveClient`, which attaches `Authorization: Bearer <token>` to every request when present.

## Offline reads

- The offline-read path deliberately avoids persistent on-disk caches. Project/asset text parses are rebuilt per request. Do not add a disk cache without explicit approval — the no-cache philosophy is documented in `docs/architecture.md`.

## Capabilities

- `src/capabilities/` holds the rule/fix catalog and the `buildCapabilities` transform. Keep them in sync when rules/fixes change (see `packages/verify/AGENTS.md` §Capability catalog sync).

## Verification

- Run `npm run typecheck` and `npm test` after changes. Tests use `node --test` with type stripping.
- Tool contract changes: update the tool catalog in `docs/api/mcp-tools.md` in the same task.
- Capability changes: verify `build-capabilities.test.ts` covers the new rule/fix/surface.

## Agent skill sync (`skills/unreal-open-mcp/SKILL.md`)

When an MCP tool, capability, route policy, or gate workflow changes, update **both** `docs/api/mcp-tools.md` and (when agent workflow is affected) `skills/unreal-open-mcp/SKILL.md`. Keep the skill lean (~150 lines); do not copy full API tables into the skill.
