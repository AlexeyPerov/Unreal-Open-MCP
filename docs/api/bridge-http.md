# Bridge HTTP API

The Unreal Open MCP bridge exposes a minimal loopback-only HTTP surface. It is
the MCP server's live entry point — every tool call eventually becomes a
loopback HTTP request. The bridge binds `127.0.0.1` only; remote bind is an
opt-in that lands later with bearer auth.

The health endpoint (`GET /ping`) is the base readiness probe. Tool dispatch
(`POST /tools/{name}`), the instance lock, and bearer auth land in later phases.

## Bind surface

| | |
|---|---|
| Address | `127.0.0.1` only. The bridge never binds a non-loopback address. Remote bind is an opt-in (later). |
| Port | Resolved from: (1) `UNREAL_OPEN_MCP_BRIDGE_PORT` CLI arg, (2) `UNREAL_OPEN_MCP_BRIDGE_PORT` env var, (3) documented default `21111`. A deterministic per-project port resolver lands in a later phase. |
| Transport | HTTP/1.1, `Connection: close` (HTTP/1.0 no keep-alive). One request per TCP connection. |
| Auth | None enforced (P1.3 scope). Bearer auth is a later opt-in. |

## Endpoints

### `GET /ping`

Health probe. The body is dispatched through the game-thread dispatcher; the
HTTP listener thread never touches UObject / editor APIs directly. A 200 means
the game thread picked the ping up within the per-call timeout. A 503 means the
game thread was blocked, the dispatcher was shut down, or the dispatch timed
out — the MCP server treats 503 as unreachable.

#### 200 response

```
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Connection: close
```

```json
{
  "connected": true,
  "status": "ready",
  "projectPath": "/abs/path/to/project",
  "unrealVersion": "5.8.0",
  "bridgeVersion": "0.0.1",
  "mode": "live",
  "port": 21111,
  "compiling": false,
  "isPlaying": false
}
```

Field order is pinned by the spec. Clients MAY probe via substring search, but
SHOULD parse the body as JSON to read individual values.

#### 503 response

Returned when the dispatcher could not run the ping on the game thread (game
thread blocked by a modal, dispatcher shut down during editor teardown, or the
dispatch timed out). Same JSON shape as the 200 path, but with `connected:false`
and `status:"not_ready"`. The MCP server treats 503 as "the editor is not ready"
and surfaces a structured unreachable state instead of waiting on a hung socket.

#### Other responses

| Status | Method | Path | Body |
|---|---|---|---|
| `405` | `POST` / `PUT` / `DELETE` / `HEAD` | `/ping` | `{"error":{"code":"method_not_allowed","message":"GET required for /ping"}}` |
| `404` | any | any other path | `{"error":{"code":"not_found","message":"Unknown path: <path>"}}` |

### Manual smoke test

```bash
curl -s http://127.0.0.1:$UNREAL_OPEN_MCP_BRIDGE_PORT/ping | jq .
```

The bridge logs `[Unreal Open MCP] bridge HTTP listening on http://127.0.0.1:<port>/ping`
on the editor Output Log at startup — that line is the proof of life when
triaging a missing /ping response.

## Planned endpoints (not shipped)

| Endpoint | Phase | Notes |
|---|---|---|
| `POST /tools/{name}` | P2.1 | Tool dispatch with gate + envelope (success/fault/timeout). |
| `GET /instance` | P1.4 | Live instance-lock JSON for the MCP server's discovery path. |
| `GET /tools` | P3.8 | Compiled-state tool inventory + group→tools map. |
| Bearer auth | P5.6 | Per-session token minted into the instance lock; `authMode: none \| required`. |
