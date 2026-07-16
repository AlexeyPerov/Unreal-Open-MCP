# Bridge HTTP API

The Unreal Open MCP bridge exposes a minimal loopback-only HTTP surface. It is
the MCP server's live entry point — every tool call eventually becomes a
loopback HTTP request. The bridge binds `127.0.0.1` only; remote bind is an
opt-in that lands later with bearer auth.

The health endpoint (`GET /ping`) is the base readiness probe and is wrapped by
the MCP `unreal_open_mcp_ping` tool (the first end-to-end live probe: stdio →
instance discovery → HTTP). Tool dispatch (`POST /tools/{name}`) is the
canonical tool-call path every typed tool family rides on. Bearer auth lands in
a later phase.

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

### `POST /tools/{name}`

Tool dispatch. The request body is the tool's arguments as a JSON object; the
bridge resolves the handler from the tool registry, runs it on the **game
thread** (never on the HTTP listener thread), and returns the canonical
`{ok, result, error}` envelope. Every typed tool family in Phase 2+ rides on
this path.

The handler's game-thread execution is marshaled through the game-thread
dispatcher with a per-call timeout. The request body's optional `timeout_ms`
field (clamped to `[1000, 600000]`, default `30000`) overrides the per-call
timeout. The `X-Agent-Id` header keys the request into the fair round-robin
queue so multiple agents sharing one bridge do not starve each other.

#### Request

```
POST /tools/unreal_open_mcp_echo HTTP/1.1
Host: 127.0.0.1
Content-Type: application/json; charset=utf-8
Content-Length: 21
X-Agent-Id: <optional agent id>

{"hello": "world"}
```

#### Success response (HTTP 200)

```json
{
  "ok": true,
  "result": { "echo": { "hello": "world" } }
}
```

`result` is the tool's own JSON value, spliced verbatim. A tool that returns
nothing carries `"result": null`. HTTP is always 200 on the success path.

#### Structured tool failure (HTTP 200)

A tool that ran but returned a failure (invalid arguments, execution error,
etc.) is reported with HTTP 200 and `ok:false` — structured tool outcomes are
never transport failures:

```json
{
  "ok": false,
  "error": {
    "code": "invalid_request",
    "message": "Missing required argument 'paths_hint'."
  }
}
```

#### Dispatch-level failures (HTTP 200)

When the game-thread dispatch itself fails (the tool never ran), the bridge
returns HTTP 200 with `ok:false` and one of these dispatch-level codes:

| Code | Cause |
|---|---|
| `game_thread_blocked` | The game thread did not pick the dispatch up within the timeout — a modal dialog or long editor operation is blocking it. |
| `timeout` | The handler started but did not finish within the per-call timeout. |
| `execution_error` | The handler threw an exception. |
| `dispatcher_shutdown` | The dispatcher is tearing down (editor quit / hot reload). |

#### Routing / transport errors

| Status | Body code | Cause |
|---|---|---|
| `404` | `tool_not_found` | No handler registered for the tool name. |
| `405` | `method_not_allowed` | Non-POST method on a tool endpoint. |
| `500` | `bridge_internal_error` | Unhandled bridge fault (rare; the body is a bare `{"error":{...}}`). |

#### Smoke stub: `unreal_open_mcp_echo`

The bridge ships a read-only echo stub registered at boot so the dispatch
round-trip is verifiable before any real tool families exist. It returns
`{"echo": <request-body>}`:

```bash
curl -s -X POST http://127.0.0.1:$UNREAL_OPEN_MCP_BRIDGE_PORT/tools/unreal_open_mcp_echo \
  -H 'Content-Type: application/json' \
  -d '{"hello":"world"}' | jq .
```

#### Envelope contract (P2.1)

The `{ok, result, error}` shape is the P2.1 canonical envelope. Gate wrapping
(checkpoint → mutate → validate → delta) is deferred to a later phase; when it
lands, the envelope will *widen* (adding gate/mutation fields) but `ok` and
`error.code` stay stable so existing parsers keep working.

### Manual smoke test

```bash
# Health probe
curl -s http://127.0.0.1:$UNREAL_OPEN_MCP_BRIDGE_PORT/ping | jq .

# Tool dispatch (echo stub)
curl -s -X POST http://127.0.0.1:$UNREAL_OPEN_MCP_BRIDGE_PORT/tools/unreal_open_mcp_echo \
  -H 'Content-Type: application/json' -d '{"smoke":1}' | jq .
```

The bridge logs `[Unreal Open MCP] bridge HTTP listening on http://127.0.0.1:<port>/ping`
on the editor Output Log at startup — that line is the proof of life when
triaging a missing /ping response.

### MCP consumer

The MCP server wraps `/ping` in the `unreal_open_mcp_ping` tool and routes every
other tool through `POST /tools/{name}`, unwrapping the `{ok, result, error}`
envelope into an MCP `CallToolResult`. An AI client calling a tool gets the
`result` value back on success, or a structured error classifying the failure so
the caller can branch on cause:

| Bridge state | MCP result |
|---|---|
| `/ping` 200 OK | success (the `/ping` body verbatim) |
| `/ping` 503 not ready | `bridge_http_error` (carries the `connected:false` fallback body) |
| `/tools/{name}` 200 `{ok:true}` | success (the `result` value verbatim) |
| `/tools/{name}` 200 `{ok:false}` | error carrying the tool's `error.code` / `error.message` |
| `/tools/{name}` 404 / 405 / 500 | error carrying the bridge's `error.code` |
| no listener / ECONNREFUSED | `bridge_offline` |
| listener accepts but never responds | `bridge_timeout` |
| 200 with non-JSON body | `bridge_response_unparsable` |

## Planned endpoints (not shipped)

| Endpoint | Phase | Notes |
|---|---|---|
| `GET /instance` | P1.4 | Live instance-lock JSON for the MCP server's discovery path. |
| `GET /tools` | P3.8 | Compiled-state tool inventory + group→tools map. |
| Bearer auth | P5.6 | Per-session token minted into the instance lock; `authMode: none \| required`. |
