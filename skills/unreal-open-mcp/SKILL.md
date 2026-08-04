# Unreal Open MCP

Practical skill for AI agents driving an Unreal Engine project through the
`unreal-open-mcp` MCP server. Every registered tool uses the prefix
`unreal_open_mcp_*`.

## Preconditions

- Unreal Editor is open with the target project when using live tools;
  `unreal_open_mcp_ping` reports `connected: true` on a healthy bridge.
- Offline reads (`read_compile_errors`, `source_read_offline`,
  `project_index`) work with the editor down — see
  [Offline reads](#offline-reads-bridge-dead).

## Non-negotiable rules

1. **Discover first** — call `unreal_open_mcp_capabilities` before assuming tool
   names, schemas, route policy, or fixes.
2. **No hardcoded bridge port** — per-project port is
   `20000 + (sha256(projectPath) % 10000)`. The instance lock `port` is the
   authority; an MCP-client config pinned `UNREAL_OPEN_MCP_BRIDGE_PORT` is the
   one known port-mismatch trap.
3. **Always scope mutations** — mutating tools require a non-empty `paths_hint`
   (content paths like `/Game/...` or source paths). Empty fails with
   `paths_hint_required`; no whole-project fallback. Set `gate:"off"` to bypass.
4. **One editor instance per project** — Unreal holds a per-project lock. Never
   launch a second editor; diagnose the wedged instance instead.
5. **Mutation success ≠ project safe** — always read `gate.delta` /
   `agentNextSteps`, even on a successful return.
6. **Failed compile is data** — `blueprint_compile` / `source_compile` return
   `ok:true` with a diagnostics list on a failed compile. Read, fix, recompile.

## Fast start sequence

1. `unreal_open_mcp_capabilities` — discover the surface (tools, verify rules,
   fixes, tool-group catalog). Call first on a fresh project.
2. Activate the tools you need via `unreal_open_mcp_manage_tools`. Sessions
   start with only the `core` group on (`ping`); the rest is hidden so the
   prompt stays lean. Toggle with `action: "list_groups" | "activate" |
   "deactivate" | "reset"` (activate/deactivate take a `group` id). A real surface
   change emits `notifications/tools/list_changed` — refresh `tools/list`. State
   is per-session and ephemeral.
   - Authoring + verifying is the common pair: activate `typed-editor` and
     `gate-and-verify`. The always-visible tools (`ping`, `capabilities`,
     `bridge_status`, `read_compile_errors`, `manage_tools`) survive even a full
     teardown.
3. `unreal_open_mcp_ping` — bridge health (or `reflection_method_find` before
   reflection-heavy calls).
4. Mutate with `gate: "enforce"` (default) + scoped `paths_hint`.
5. On gate failure, prefer `unreal_open_mcp_apply_fix` with `dry_run: true`
   first; re-run and confirm `gate.delta.newErrors == 0` or `resolvedErrors > 0`.

## Core loop: mutate → gate → fix

1. **Discover** — `capabilities`; then `reflection_method_find` for reflection
   targets, `actor_find` / `asset_find` / `level_get_data` to scope the edit.
2. **Declare scope** — `paths_hint` for every content/source path you intend to
   touch.
3. **Mutate** — typed tools preferred over `reflection_method_call` /
   `console_run_command`; default `gate: "enforce"`.
4. **Read the gate** — on `gate.outcome != "passed"`, inspect
   `gate.delta.newIssueKeys` + `agentNextSteps`.
5. **Fix** — address the top error; `apply_fix` with `dry_run: true` first when a
   `fixId` is present. Each issue carries `ruleId` / `severity` / `code` /
   `assetPath` / `description` / `evidence`.
6. **Retry** — confirm `gate.delta.resolvedErrors > 0` or `newErrors == 0`.

### Gate modes and outcome

| Mode                | When                                                    |
| ------------------- | ------------------------------------------------------- |
| `enforce` (default) | Normal edits — fail fast on new Errors                  |
| `warn`              | Exploratory — read `gate.delta` but call does not error |
| `off`               | Trusted admin only — no checkpoint/validate             |

Mutating dispatches widen the envelope with a `gate` block. Branch on
`gate.outcome`: `passed` (no new Errors) · `warned` (new Errors under Warn, or
new Warnings) · `failed` (new Errors under Enforce, mutation failed, or key
validation failed) · `skipped` (`off`/read-only) · `validate_scan_failed`
(scan threw after a committed mutation — run `validate_edit`). `gate.delta`
carries `newErrors`/`newWarnings`/`resolvedErrors`/`resolvedWarnings` counts
plus `newIssueKeys`/`resolvedIssueKeys`. Issue keys are
`{ruleId}|{severity}|{assetPath}|{issueCode}`; fetch the full per-instance list
via `validate_edit`.

### Verify rules and issue codes

Authoritative via `capabilities`. Implemented:

- **`broken_soft_references`** — walks `FSoftObjectPath` properties; checks each
  target resolves. Code `broken_soft_reference[:<suffix>]` (Error). The one rule
  with a shipped fix.
- **`missing_blueprint_parents`** — reads `UBlueprint::ParentClass`; null parent
  after `LoadPackage` reports `expectedParent="(unknown)"`. Code
  `missing_blueprint_parent[:<expected-parent>]` (Error).
- **`compile_errors`** — current compile state + diagnostics; never triggers a
  compile. Code `compile_error[:<file>:<line>]` (Error).

Rule auto-selection by extension: `.uasset`/`.umap` → all three;
`.cpp`/`.h`/`.cs` → `compile_errors`; unknown/empty → every registered rule.
Override via `categories` / `include_rules` / `exclude_rules` on `validate_edit`.

### Fixes

`apply_fix` defaults to `dry_run: true` (dry-run short-circuits the gate — no
checkpoint/validate, no rollback snapshot). Implemented:

- **`clear_broken_soft_reference`** (Safe) — nulls a broken soft object pointer
  at a precise top-level property path and saves the package. Refuses
  struct-nested properties in v1 (`safe: false`).

If `fix_id` is omitted, the response lists every fix that can resolve the
`issue_id`. A non-dry-run apply runs through the gate runner: on failure or new
Errors under `enforce`, touched files are restored and the response carries
`rollback: { rolledBack, reason, restoredPaths[] }`. With `gate: "off"` rollback
protection is off (`rollback.rollbackDisabled: true`).

## Offline reads (bridge dead)

When the bridge is dead (editor not running, bridge module failed to compile —
Live Coding failure / a bad C++ edit — or a crash), three tools route `offline`
and read disk, never touching the live transport.

- **`read_compile_errors`** — the **one channel that survives a dead bridge
  assembly**. Reads the newest `<Project>/Saved/Logs/*.log` tail, parses
  MSVC/clang diagnostics into `{ file, line, severity, message }[]`. Tokens:
  `log_not_found` (non-error), `no_errors_found`.
- **`source_read_offline`** — offline twin of `source_read`; same
  `<Project>/Source/` jail.
- **`project_index`** — `.uproject` basics + optional `Source`/`Config`/
  `Content` listing (text extensions only).

Offline reads need `UNREAL_PROJECT_PATH` bound (else `project_path_not_bound`).
**Binary `.uasset`/`.umap` assets are not parsed offline** — they need the live
bridge. No persistent cache; parsers rebuild per request.

## Unreal state triage (before edits, and on `bridge_offline`)

The single most common mistake is misclassifying Unreal's state. Follow this
**in order** before mutating, and whenever a tool returns `bridge_offline`.

> **Editing C++?** An offline bridge after a source edit is frequently a
> *symptom* of a failed Live Coding compile, not "the editor isn't running." Go
> straight to [Compile failure recovery](#compile-failure-recovery) and call
> `read_compile_errors`.

1. **Let the server tell you.** `bridge_status` (or `ping`) composes the
   instance-lock classifier with a `/ping` probe and reports `dead_bridge` with
   a recovery hint → `read_compile_errors`. A dead bridge is a successful status
   read, never an error.
2. **Read the instance lock.**
   `~/.unreal-open-mcp/instances/<sha256(projectPath)>.json` carries `pid`,
   `port`, `state` (`idle`/`compiling`/`reloading`/`entering_playmode`/`playing`/
   `exiting_playmode`), `heartbeatAt` (stale > 10s + live pid ⇒ dead bridge),
   `unrealVersion`, `authToken`. Check pid liveness (`kill -0 <pid>`); trust the
   lock's `port` over any env var.
3. **Port mismatch.** A healthy lock but every tool `bridge_offline` usually
   means an MCP-client config pinned `UNREAL_OPEN_MCP_BRIDGE_PORT` to a stale
   value. Tell the operator the actual port; removing the env var lets both sides
   derive the per-project hash and agree.

### Compile failure recovery

Compile failures surface as **machine-readable codes** — branch on the code:

- **`bridge_offline`** — no listener / `ECONNREFUSED`. The editor may not be
  running, or Live Coding/UBT failed mid-recompile. If the editor is open it has
  written the latest diagnostics to the log; `read_compile_errors` retrieves
  them.
- **`game_thread_blocked`** — a modal dialog or long editor operation blocked
  the dispatch; it **never started**, so do NOT raise `timeout_ms`. Dismiss the
  modal (or ask the operator), check `bridge_status`, retry.
- **`timeout`** — the handler started but ran long; this one CAN warrant a
  higher `timeout_ms`.
- **`paths_hint_required`** — mutating call omitted `paths_hint`. Re-issue with
  it scoped to the touched paths, or set `gate: "off"`.

For `bridge_offline` after a C++ edit: `read_compile_errors` → fix
`errors[].file`/`line` → `source_compile`.

## Typed tool families

Prefer these over `reflection_method_call` / `console_run_command` — explicit
schemas, same gate envelope, structured results. All mutating tools accept
`gate` (`enforce`/`warn`/`off`, default `enforce`) and require a non-empty
`paths_hint`. Full per-tool detail lives in `capabilities`; route source of
truth is `mcp-server/src/tool-router.ts`. `(read)` marks gate-free tools.

- **Actors** — `actor_find` (read), `actor_create`, `actor_modify`,
  `object_modify`, `actor_set_parent`, `actor_duplicate`, `actor_destroy`.
- **Components** — `actor_component_get` / `_list_all` (read),
  `actor_component_add` / `_destroy` / `_modify`.
- **Levels** — `level_list_loaded` / `level_get_data` (read), `level_open`,
  `level_save`, `level_set_current`, `level_unload_sublevel`, `level_create`.
- **Assets** — `asset_find` / `asset_get_data` / `asset_refresh` (read),
  `asset_create_folder`, `asset_copy`, `asset_move`, `asset_delete`,
  `asset_import`. Content paths are `/Game/...`; levels are `.umap`, assets are
  `.uasset`.
- **Materials** — `material_get_data` (read), `material_create`,
  `material_modify`.
- **Blueprints** — `blueprint_get` (read), `blueprint_create`,
  `blueprint_add_component` / `_remove_component`, `blueprint_add_variable` /
  `blueprint_modify_variable`, `blueprint_set_default`, `blueprint_add_function`
  / `_add_event`, `blueprint_compile`, `blueprint_spawn`.
- **Source (C++)** — `source_read` / `source_list` (read),
  `source_create_class`, `source_update`, `source_delete`, `source_compile`.
- **Editor / console / reflection / screenshot** — `editor_application_*`,
  `editor_selection_*`, `console_get_logs` (read), `console_clear_logs`,
  `console_run_command`, `reflection_method_find` (read), `reflection_method_call`,
  `screenshot_viewport` / `_game_view` / `_camera` / `_isolated` (all read).
- **Gate meta-tools** — `validate_edit` / `checkpoint_create` / `delta` (read,
  bypass `GatePolicy.Execute`), `apply_fix` (mutating, dry-run by default).

### Checkpoint → mutate → delta (large refactors)

`checkpoint_create` with scoped paths → mutations (`gate: "off"` for bulk, or
`enforce` per call) → `delta` for one verification pass. Checkpoints are
session-scoped (in-memory), wiped on hot reload / restart. A missing checkpoint
is non-blocking: `delta` returns `{ ok:true, unavailable:true, agentNextSteps[] }`
— fall back to `validate_edit`.

## Routing

Treat `capabilities.routePolicy` as source of truth. **Live is the default**
(`POST /tools/{name}`). **Offline** reads parse from disk. **Local**
(`capabilities`, `bridge_status`, `manage_tools`) resolves in-process. **Batch**
(headless commandlet) is planned, not implemented — returns
`batch_not_implemented`; mutating meta-tools (`reflection_method_call`,
`console_run_command`) have no batch form and need a live editor.

## Agent checklist

**Before mutating**

- [ ] `capabilities` refreshed; bridge state classified (`bridge_status` / lock
      read — pid + heartbeat checked); `paths_hint` prepared.

**After mutating**

- [ ] `gate.delta` reviewed (branch on `gate.outcome`); fixes applied / retried
      (`apply_fix` `dry_run:true` first).
- [ ] Compile verified (`blueprint_compile` / `source_compile` clean, or
      `read_compile_errors` if bridge down).
