# Bridge HTTP API

The Unreal Open MCP bridge exposes a minimal loopback HTTP surface by default.
It is the MCP server's live entry point — every tool call eventually becomes a
loopback HTTP request. The bridge binds `127.0.0.1` by default; a remote bind
is an opt-in that requires bearer auth (see [Remote bind](#remote-bind)).

The health endpoint (`GET /ping`) is the base readiness probe and is wrapped by
the MCP `unreal_open_mcp_ping` tool (the first end-to-end live probe: stdio →
instance discovery → HTTP). Tool dispatch (`POST /tools/{name}`) is the
canonical tool-call path every typed tool family rides on. A bearer auth gate
runs on every endpoint when `authMode` is `"required"` (see [Auth](#auth)).

## Bind surface

| | |
|---|---|
| Address | `127.0.0.1` by default. Remote bind (`0.0.0.0`) is an opt-in that requires `authMode: "required"` (see [Remote bind](#remote-bind)). |
| Port | Resolved from: (1) `UNREAL_OPEN_MCP_BRIDGE_PORT` env var, (2) `-UNREAL_OPEN_MCP_BRIDGE_PORT=<n>` CLI arg, (3) deterministic per-project hash `20000 + (sha256(projectPath) % 10000)`. |
| Transport | HTTP/1.1, `Connection: close` (HTTP/1.0 no keep-alive). One request per TCP connection. |
| Auth | None enforced by default (`authMode: "none"`). Opt-in bearer-token gate via `authMode: "required"` (see [Auth](#auth)). |

## Auth

The bridge mints a per-session bearer token on startup and writes it into the
instance lock JSON as `authToken` (field #3, between `port` and `projectPath`).
The token is a 64-char lowercase-hex string (256 bits of entropy), rotated on
every bridge start so a restart invalidates any previously discovered token.
Enforcement is opt-in via `authMode` in
[project settings](#project-settings):

| `authMode` | Behavior |
|---|---|
| `"none"` (default) | Open. Any request is allowed regardless of the `Authorization` header. The token is still minted + advertised so a project can flip to `"required"` with no restart. |
| `"required"` | Every request MUST carry `Authorization: Bearer <token>` matching the lock's `authToken`, or receive HTTP 401. |
| (anything else) | Fail closed — treated as deny. An unrecognized value (null/corrupt/typo) never silently opens the bridge. |

The auth gate runs on **every** endpoint before routing — `/ping`,
`/tools/{name}`, and any future endpoint. No endpoint is exempt. Token
comparison is constant-time (no early-exit timing leak).

A 401 response carries the stable `unauthorized` code:

```json
{
  "error": {
    "code": "unauthorized",
    "message": "Missing or invalid Authorization header. Set authMode to \"none\" in .unreal-open-mcp/settings.json, or send Authorization: Bearer <token>."
  }
}
```

The MCP server auto-discovers the token from the live instance lock
(`resolveAuthToken` in `instance-discovery.ts`) and attaches
`Authorization: Bearer <token>` to every request when a token is present. When
no lock exists (e.g. an `UNREAL_OPEN_MCP_BRIDGE_PORT` override bypasses the
lock), no header is sent and the bridge must be in `authMode: "none"`. This is
fully automatic — operators do not need to configure the token on the MCP side.

### Project settings

The bridge reads `<project-root>/.unreal-open-mcp/settings.json` at startup.
Schema (only the fields the bridge reads today):

```json
{
  "authMode": "none",
  "bindAddress": "127.0.0.1"
}
```

| Field | Values | Default | Notes |
|---|---|---|---|
| `authMode` | `"none"` \| `"required"` | `"none"` | Invalid values coerce to `"none"` at load. |
| `bindAddress` | `"127.0.0.1"` \| `"0.0.0.0"` | `"127.0.0.1"` | Invalid values coerce to loopback (never remote). Remote requires `authMode: "required"`. |

A missing file, an unparseable JSON body, or an invalid value all fall back to
the safe defaults (loopback + open) — the bridge never fails to start over a
malformed settings file.

### Remote bind

A remote bind (`bindAddress: "0.0.0.0"`) opens the bridge to the network. To
avoid an unauthenticated open port, the bridge **refuses to start** on a remote
interface unless `authMode` is `"required"`:

```log
[Unreal Open MCP] bridge HTTP start refused: Remote bind (0.0.0.0) requires
authMode "required". ... set authMode to "required" in
.unreal-open-mcp/settings.json before enabling remote bind.
```

This couples the bind decision to the auth policy so a misconfigured project
fails fast at startup instead of exposing an open port. Loopback binds are
always allowed regardless of `authMode`. An invalid `bindAddress` coerces to
loopback (never remote).

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
| `paths_hint_required` | (Mutating tools only) The request omitted `paths_hint`. The gate refuses to fall back to a whole-project scan; re-issue with `paths_hint` or set `gate:"off"`. |

#### Routing / transport errors

| Status | Body code | Cause |
|---|---|---|
| `404` | `tool_not_found` | No handler registered for the tool name. |
| `405` | `method_not_allowed` | Non-POST method on a tool endpoint. |
| `500` | `bridge_internal_error` | Unhandled bridge fault (rare; the body is a bare `{"error":{...}}`). |

## Gate policy

The gate is the bridge's core safety contract for mutating tools. Every mutating
dispatch routes through `FUnrealOpenMcpGatePolicy::Execute` — a single
mandatory chokepoint that runs `checkpoint → mutate → validate → delta` around
the tool handler. Read-only tools dispatch directly (gate Off) and never enter
the gate path.

### `paths_hint` is mandatory for mutating tools

A mutating request MUST include a non-empty `paths_hint: string[]` (content /
source paths the mutation is scoped to). The hint bounds both the pre-mutation
checkpoint and the post-mutation validate scan. An empty hint fails fast with
the structured `paths_hint_required` error BEFORE any mutation runs — there is
no whole-project fallback scan (Unity parity; pinned by
`packages/bridge/AGENTS.md` §Gate policy). Set `gate:"off"` to bypass the gate
and skip the hint.

### Gate precedence

| Step | Source | Notes |
|---|---|---|
| 1 | Request `gate` | One of `"enforce"`, `"warn"`, `"off"`. Case-sensitive; unknown values fall back to Enforce. |
| 2 | Tool default | `Enforce` for mutating tools (registry metadata); `Off` for read-only tools. |

A later phase adds step 3 (project default from `.unreal-open-mcp/settings.json`).
Until then, the tool default is the fallback.

### Gate outcome

The gate resolves one of five outcomes, surfaced as the wire token
`gate.outcome`:

| Outcome | Meaning |
|---|---|
| `passed` | Mutation committed; post-mutation delta reported no new Errors (warnings may be present). |
| `warned` | Mutation committed; delta reported new Errors under Warn mode OR new Warnings surfaced. `gate.gateFailed` stays false (the mutation is in). |
| `failed` | Enforce-mode delta reported new Errors, OR the mutation itself failed, OR checkpoint/delta key validation failed. The dispatch is non-passing; the agent must fix and retry. |
| `skipped` | Gate did not run (Off mode, empty paths_hint, or read-only tool). Outcome follows the mutation result. |
| `validate_scan_failed` | Mutation committed but the post-mutation validate scan threw — distinct from `failed` (the delta did not report new errors; the scanner blew up). The agent should run `unreal_open_mcp_validate_edit` / `unreal_open_mcp_scan_paths` to confirm health. |

### Widened envelope (mutating dispatches)

Mutating dispatches widen the P2.1 `{ok, result, error}` envelope with a `gate`
summary so an agent can branch on the gate decision without parsing prose in
`agentNextSteps`. Read-only tools keep the P2.1 shape exactly — the widening
only adds fields, never renames or removes.

Mutating success (HTTP 200):

```json
{
  "ok": true,
  "result": { "actor": { "label": "PointLight_1", "path": "..." } },
  "gate": {
    "ran": true,
    "outcome": "passed",
    "gateFailed": false,
    "checkpointId": "cp_abcdef",
    "delta": {
      "newErrors": 0,
      "newWarnings": 0,
      "resolvedErrors": 0,
      "resolvedWarnings": 0,
      "newIssueKeys": [],
      "resolvedIssueKeys": []
    },
    "categoriesRun": ["broken_soft_references", "missing_blueprint_parent", "compile_errors"],
    "checkpointMs": 12,
    "validateMs": 47,
    "totalMs": 65,
    "agentNextSteps": ["Gate passed — no new issues detected."]
  }
}
```

Mutating failure (HTTP 200, e.g. ValidateScanFailed):

```json
{
  "ok": false,
  "error": { "code": "execution_error", "message": "..." },
  "gate": {
    "ran": true,
    "outcome": "validate_scan_failed",
    "gateFailed": true,
    "checkpointId": "cp_abcdef",
    "agentNextSteps": [
      "Mutation committed, but the gate's validate scan threw (...). Run unreal_open_mcp_validate_edit on the touched paths to confirm health."
    ]
  }
}
```

The `paths_hint_required` body adds an `effectiveMode` field so the agent sees
which gate mode was applied when the hint was missing:

```json
{
  "ok": false,
  "error": {
    "code": "paths_hint_required",
    "message": "Mutating tool 'unreal_open_mcp_actor_create' requires a non-empty paths_hint..."
  },
  "gate": { "ran": false, "outcome": "failed", "gateFailed": true, "effectiveMode": "enforce" }
}
```

### Image envelope (screenshot tools)

The screenshot family (`screenshot_viewport` / `screenshot_game_view` /
`screenshot_camera` / `screenshot_isolated`) widens the success envelope with a
top-level `image` field carrying the base64 PNG. The MCP server's `LiveClient`
unwraps `image` into a dedicated MCP image content block, so the base64 is
returned to the agent as viewable image content — never a text dump. Every
non-screenshot tool's envelope is byte-identical to before (no `image` field).

Image success (HTTP 200):

```json
{
  "ok": true,
  "result": {
    "source": "editor viewport",
    "width": 1920,
    "height": 1080,
    "mimeType": "image/png",
    "byteSize": 2048576
  },
  "image": {
    "data": "iVBORw0KGgoAAAANSUhEUgAA...",
    "mediaType": "image/png"
  }
}
```

- `result` is the metadata object (`source`, `width`, `height`, `mimeType`,
  `byteSize`). `source` distinguishes the origin (`editor viewport`,
  `PIE game view`, `camera '<name>'`, `isolated actor '<name>'`).
- `image.data` is the raw base64 PNG (no `data:` URI prefix).
- `image.mediaType` is always `"image/png"` for the screenshot family.

Caps + guarantees (all four tools):

- Dimensions clamped to `[1, 2048]` per side; default 1024 (viewport/game-view
  default to the native size when both are omitted, deriving the other axis
  from the native aspect when only one is given).
- Pixels forced opaque (alpha → 255) so a viewport/render-target alpha of 0
  never yields a transparent PNG.
- Encoded byte cap 40 MiB — exceeded captures return `image_too_large` rather
  than truncating the IPC frame.
- Transient capture actors + render targets are cleaned up on every return
  path (`ON_SCOPE_EXIT` destroy + `FGCObjectScopeGuard`); the editor world is
  never dirtied.

Screenshot error codes (structured, returned via the standard `{ok:false,
error:{code,message}}` shape):

| Code | Cause |
|---|---|
| `invalid_parameter` | Malformed body, or (isolated) a malformed `background` hex. |
| `missing_parameter` | (camera) `camera` absent, or (isolated) `actor` absent. |
| `actor_not_found` | (camera/isolated) the actor ref did not resolve. |
| `no_editor_world` | (camera/isolated) no editor world available. |
| `rendering_unavailable` | Headless / `-nullrhi` — capture requires a GPU-backed (windowed) editor. |
| `editor_unavailable` | (viewport) no active editor viewport. |
| `pie_not_running` | (game-view) no PIE session is active. |
| `capture_failed` | Zero-sized viewport, spawn failure, or read-back failure. |
| `image_too_large` | Encoded PNG exceeds the 40 MiB byte cap — request a smaller width/height. |

#### Smoke stub: `unreal_open_mcp_echo`

The bridge ships a read-only echo stub registered at boot so the dispatch
round-trip is verifiable before any real tool families exist. It returns
`{"echo": <request-body>}`:

```bash
curl -s -X POST http://127.0.0.1:$UNREAL_OPEN_MCP_BRIDGE_PORT/tools/unreal_open_mcp_echo \
  -H 'Content-Type: application/json' \
  -d '{"hello":"world"}' | jq .
```

#### Envelope contract (P2.1 + P3.5)

The `{ok, result, error}` shape is the P2.1 canonical envelope. P3.5 widened it
for mutating dispatches by adding a `gate` summary block (see
[Gate policy](#gate-policy)). Read-only tools keep the P2.1 shape exactly;
`ok` and `error.code` stay stable across both forms so a P2.1 parser keeps
working.

#### Gate meta-tools (P3.6)

The bridge ships three read-only meta-tools that surface the explicit
**checkpoint → mutate → delta** workflow agents drive when they want a manual
gate pass. All three are registered as non-mutating with gate `Off` so the
dispatch policy runs them directly — they participate in the gate workflow
but do not recurse through `FUnrealOpenMcpGatePolicy::Execute`.

| Tool | Purpose |
|---|---|
| `unreal_open_mcp_validate_edit` | Scoped health check without a preceding mutation (manual verification / pre-commit). |
| `unreal_open_mcp_checkpoint_create` | Capture a verify fingerprint for later delta comparison. |
| `unreal_open_mcp_delta` | Compare current project health vs a stored checkpoint. |

The canonical workflow is:

```bash
# 1. Capture a baseline before mutating.
CP=$(curl -s -X POST http://127.0.0.1:$UNREAL_OPEN_MCP_BRIDGE_PORT/tools/unreal_open_mcp_checkpoint_create \
  -H 'Content-Type: application/json' \
  -d '{"paths":["/Game/BP/BP_Foo.BP_Foo"],"label":"before-edit"}' \
  | jq -r .result.checkpointId)

# 2. Mutate (any mutating tool: actor_create, actor_modify, level_save, …).
#    paths_hint should match the checkpoint scope so the post-mutation
#    validate scan is bounded to the touched paths.

# 3. Delta against the baseline.
curl -s -X POST http://127.0.0.1:$UNREAL_OPEN_MCP_BRIDGE_PORT/tools/unreal_open_mcp_delta \
  -H 'Content-Type: application/json' \
  -d "{\"checkpoint_id\":\"$CP\"}" | jq .
```

`validate_edit` returns the issue list with stable fields:

```json
{
  "passed": false,
  "issues": [
    {
      "ruleId": "broken_soft_references",
      "categoryId": "broken_soft_references",
      "severity": "Error",
      "code": "broken_soft_reference",
      "issueCode": "broken_soft_reference",
      "assetPath": "/Game/BP/BP_Foo.BP_Foo",
      "description": "Soft object path '/Game/MissingAsset.MissingAsset' does not resolve.",
      "evidence": { "property": "Mesh.SkeletalMesh" }
    }
  ],
  "categoriesRun": ["broken_soft_references", "missing_blueprint_parent", "compile_errors"],
  "rulesApplied": ["broken_soft_references", "missing_blueprint_parent", "compile_errors"],
  "durationMs": 42
}
```

`passed` follows the strict-error contract: validate_edit is the gate's
pre-mutation check, so it fails on any `Error`. The project severity threshold
flows into the regression gate; here the contract is strict-error because
validate_edit answers "is this asset currently healthy?".

**Rule auto-select** (Unreal extension map):

| Extension | Rules selected |
|---|---|
| `.uasset`, `.umap` | `broken_soft_references`, `missing_blueprint_parent`, `compile_errors` |
| `.cpp`, `.h`, `.cs` | `compile_errors` |
| (unknown / empty) | fallback: every registered rule |

The caller can override via `categories` (explicit rule list), narrow via
`include_rules` (intersect with the explicit list, or additive when
`categories` is omitted), or filter via `exclude_rules` (deny-list; always
wins).

**Missing-checkpoint path.** Checkpoints are session-scoped (in-memory) and
are wiped on hot reload or editor restart. `delta` does NOT treat a missing
checkpoint as a tool failure — it returns `{ok:true}` with an explicit
`unavailable` payload so an agent can proceed (e.g. fall back to
`validate_edit`):

```json
{
  "passed": true,
  "unavailable": true,
  "checkpointLostOnReload": true,
  "warning": "Checkpoint 'cp_deadbeef' was not found and the in-memory checkpoint store is empty...",
  "agentNextSteps": [
    "The checkpoint store is empty — the pre-change baseline is gone (or was never created) and a delta cannot be computed.",
    "To verify current state directly, call unreal_open_mcp_validate_edit (or unreal_open_mcp_scan_paths) on the relevant paths.",
    "To re-establish a baseline, call unreal_open_mcp_checkpoint_create on the paths you intend to delta-check, then mutate, then unreal_open_mcp_delta."
  ]
}
```

`checkpointLostOnReload` is set ONLY when the store is completely empty AND a
specific id was requested (the most likely cause is a hot reload that wiped
the store). When the id is unknown but other checkpoints still exist, the
payload carries `unavailable:true` without `checkpointLostOnReload` so the
agent can distinguish "wiped by reload" from "this id was never created in
this session".

#### apply_fix (P3.7)

`unreal_open_mcp_apply_fix` is the fix application workflow. Dry-run previews
the fix without mutating (no gate, no rollback snapshot); non-dry-run applies
run through the bridge's `ApplyFixGateRunner` so a `FixRollback` snapshot
protects the asset and a corrupting fix is auto-reverted on failure or new
errors under `enforce`.

**Args:**

| Field | Required | Description |
|---|---|---|
| `issue_id` | yes | Canonical issue key (`{ruleId}\|{severity}\|{assetPath}\|{issueCode}`). The asset-path component is auto-derived as the gate's `paths_hint` when `paths_hint` is omitted. |
| `fix_id` | no | Fix action id from the issue payload (e.g. `clear_broken_soft_reference`). When omitted, the response lists every fix that can resolve the issue. |
| `dry_run` | no | Default `true`. Preview the fix without applying. Dry-run short-circuits the gate entirely. |
| `gate` | no | One of `enforce` (default) / `warn` / `off`. Ignored for `dry_run`. Use `gate:"off"` only when you accept the fix commits with no rollback — the response then carries `rollbackDisabled:true`. |
| `paths_hint` | no | Optional mutation scope override. Auto-derived from `issue_id`'s asset path when omitted. |

**Result shapes:**

```json
// Dry-run with fix_id
{ "dryRun": true, "fixId": "clear_broken_soft_reference", "issueId": "...",
  "assetPath": "/Game/Foo/Bar.Bar", "description": "Clear the broken soft object pointer at 'Weapon' on '/Game/Foo/Bar.Bar' (sets the property to null and saves the package).",
  "safe": true }

// Dry-run without fix_id — list every fix that can resolve the issue
{ "dryRun": true, "issueId": "...", "availableFixIds": ["clear_broken_soft_reference"] }

// Apply succeeded
{ "dryRun": false, "success": true, "description": "Cleared 1 broken soft object pointer(s) at 'Weapon' on '/Game/Foo/Bar.Bar' and saved package '/Game/Foo/Bar'.",
  "touchedPaths": ["/Game/Foo/Bar.Bar"] }
```

**Safe auto-fix rollback.** A non-dry-run apply that fails OR introduces new
errors under `enforce` is restored to its pre-fix state. The success envelope
adds a top-level `rollback` block:

```json
{
  "ok": true,
  "result": { "dryRun": false, "success": false, "description": "..." },
  "gate": { "ran": true, "outcome": "failed", "gateFailed": true, "delta": { "newErrors": 1, ... } },
  "rollback": {
    "rolledBack": true,
    "reason": "fix introduced 1 new error(s) under enforce — restored touched files to pre-fix state",
    "restoredPaths": ["/Game/Foo/Bar.Bar"]
  }
}
```

`gate:"off"` skips the delta and never consults the rollback snapshot — a fix
that corrupts the asset under this path is permanent. The response carries
`rollback.rollbackDisabled:true` so an agent knows the mutation committed
without auto-rollback protection:

```json
{
  "ok": true,
  "result": { "dryRun": false, "success": true, "description": "...", "touchedPaths": [...] },
  "gate": { "ran": false, "outcome": "skipped", "gateFailed": false },
  "rollback": { "rolledBack": false, "rollbackDisabled": true }
}
```

**Error codes:**

| Code | Cause |
|---|---|
| `missing_parameter` | `issue_id` absent. |
| `invalid_issue_id` | `issue_id` is not a well-formed `{ruleId}\|{severity}\|{assetPath}\|{issueCode}` key. |
| `unknown_fix` (structured, `ok:true`) | `fix_id` does not resolve to a registered provider. Body carries `availableFixIds` + `applicableFixIdsForIssue` for self-correction. |
| `fix_not_applicable` | The named fix's `CanFix()` rejected the issue id. |
| `rollback_unavailable` | A non-dry-run apply was dispatched without the `ApplyFixGateRunner` wrapper (e.g. via `batch_execute`). Re-issue as a top-level call with `gate != off`, or use `dry_run:true`. |
| `fix_failed` | The provider's `Apply()` returned `!success` (e.g. ambiguous target, asset layout changed since scan). The gate runner rolls back under `enforce`. |
| `fix_error` | The provider threw during `Apply()` (only fireable in `WITH_EXCEPTIONS` builds). |

**v1 implemented fix:** `clear_broken_soft_reference` (Safe) — nulls a broken
soft object pointer at a precise top-level property path and saves the
package. Refuses struct-nested properties in v1 (`Describe()` returns
`safe:false` so the gate never auto-suggests them). The provider keys off the
`broken_soft_references` rule + the `broken_soft_reference` issue code with a
`<targetPackage>:<propertyPath>` suffix.

### Manual smoke test

```bash
# Health probe
curl -s http://127.0.0.1:$UNREAL_OPEN_MCP_BRIDGE_PORT/ping | jq .

# Tool dispatch (echo stub)
curl -s -X POST http://127.0.0.1:$UNREAL_OPEN_MCP_BRIDGE_PORT/tools/unreal_open_mcp_echo \
  -H 'Content-Type: application/json' -d '{"smoke":1}' | jq .

# Auth (when authMode is "required"): the token is in the instance lock.
TOKEN=$(jq -r .authToken ~/.unreal-open-mcp/instances/*.json | head -1)
curl -s http://127.0.0.1:$UNREAL_OPEN_MCP_BRIDGE_PORT/ping \
  -H "Authorization: Bearer $TOKEN" | jq .
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
| `/tools/{name}` 200 `{ok:true}` (read-only) | success (the `result` value verbatim — P2.1 shape) |
| `/tools/{name}` 200 `{ok:true,...,"gate":{...}}` (mutating) | success; `result` is the primary payload and `gate` rides through as metadata so an agent can branch on `gate.outcome` |
| `/tools/{name}` 200 `{ok:false}` | error carrying the tool's `error.code` / `error.message`; when a `gate` block is present it rides through as `detail.gate` |
| `/ping` or `/tools/{name}` 401 | error carrying `unauthorized` (only when `authMode: "required"` and the Bearer token is missing/wrong) |
| `/tools/{name}` 404 / 405 / 500 | error carrying the bridge's `error.code` |
| no listener / ECONNREFUSED | `bridge_offline` |
| listener accepts but never responds | `bridge_timeout` |
| 200 with non-JSON body | `bridge_response_unparsable` |

## Planned endpoints (not shipped)

| Endpoint | Notes |
|---|---|
| `GET /instance` | Live instance-lock JSON for the MCP server's discovery path. |
| `GET /tools` | Compiled-state tool inventory + group→tools map. |
