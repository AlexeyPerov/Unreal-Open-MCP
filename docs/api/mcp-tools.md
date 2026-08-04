# MCP tools API

`unreal-open-mcp` exposes a typed, safety-gated tool surface for Unreal Engine
projects. This page is the per-tool route catalog — how every shipped tool is
dispatched (`live` / `offline` / `local` / `batch`) — plus the contract notes
that govern mutation, the gate, and offline coverage.

For the loopback bridge endpoints and the `{ok, result, error}` envelope, see
[Bridge HTTP](bridge-http.md). For repository boundaries and the dispatch flow,
see [Architecture](../architecture.md).

For exact runtime schemas, call `unreal_open_mcp_capabilities`. Source
definitions live in `mcp-server/src/tools/`.

> **Source of truth.** The route classification for every tool is the policy
> table in `mcp-server/src/tool-router.ts` (`routePolicy`). This catalog mirrors
> that table; when a tool's route changes, both are updated in the same change.

## Route legend

The MCP server's `ToolRouter` dispatch spine (`mcp-server/src/tool-router.ts`)
classifies every `tools/call` name into exactly one route, then stamps the
choice as metadata on the JSON result so an agent (or the integration suite) can
branch on where a response came from without scraping prose.

| Route | Meaning | `_source` | `_route.route` |
|---|---|---|---|
| `live` | POST to the running Unreal Editor bridge (`GET /ping` for ping; `POST /tools/{name}` for everything else). Default for any name not classified offline/local/batch. | `live` | `live` |
| `offline` | Read the project tree from disk with the editor DOWN — no bridge hop. Project files, source text, and the editor log only. | `offline` | `offline` |
| `local` | Resolve in-process in the MCP server — no bridge round-trip. | `local` | `local` |
| `batch` | Headless Unreal commandlet. Recognized but refuses with `batch_not_implemented` until the spawn lands. | — | `batch` |

`_source` + `_route: { route }` are stamped on **every** JSON result — success
**and** error — so agent recovery logic can trust them. A tool classified
offline/local/batch without a matching in-process handler surfaces a structured
`offline_handler_missing` / `local_handler_missing` error (never a silent live
fallthrough).

## Tool groups

Every registered tool maps to a **group id** via the canonical catalog in
`mcp-server/src/capabilities/tool-groups.ts`. A fresh session advertises only
the default-on group (`core`) plus a handful of always-visible meta / recovery
tools; every other family activates on demand via `unreal_open_mcp_manage_tools`.
See [Tool groups and session visibility](tool-groups.md) for the activation
model and per-session state.

The family sections below are organised by tool family; use this map to find
each family's visibility bucket:

| Group | Default | Families |
|---|---|---|
| `core` | **on** | ping — the connectivity probe. |
| `gate-and-verify` | off | Gate & validation meta-tools (validate / checkpoint / delta / apply_fix). |
| `typed-editor` | off | Actor, actor-component, level, asset, material, editor, console, reflection, screenshot, Blueprint, source. |
| _always-visible (no group)_ | — | `capabilities`, `bridge_status`, `manage_tools` (local) + the offline recovery surface (`read_compile_errors`, `source_read_offline`, `project_index`). Reachable even with every group torn down. |
| `diagnostics` | off | Reserved — empty today; future profiler / per-frame reads. |

## Route policy by tool

Every shipped tool has one route. The `Mutating` column records whether the tool
writes editor/project state (and so participates in the gate contract below).
Mutating tools require a non-empty `paths_hint` and default to gate `enforce`;
read-only tools are gate-free.

### local (3)

Resolved in-process, no bridge round-trip.

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| `unreal_open_mcp_capabilities` | local | no | Discovery surface — tools + verify rules + fixes in one call. |
| `unreal_open_mcp_bridge_status` | local | no | Operator health snapshot — composes the instance-lock classifier with one `/ping` probe. A dead/stopped bridge is a successful status read, never an error. `dead_bridge` recovery hint points at `read_compile_errors`. |
| `unreal_open_mcp_manage_tools` | local | no | Per-session tool-group visibility mutator (`list_groups` / `activate` / `deactivate` / `reset`). Always-visible so an agent can re-activate a torn-down group. Not a gate mutator — it touches session visibility only, never editor/project state. Emits `notifications/tools/list_changed` when the visible surface changes; no-op actions do not emit. |

### offline (3)

Disk readers resolved with the editor DOWN. Never touch the live transport.
Scope: project files, source text, editor log — **no** `.uasset` parse.

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| `unreal_open_mcp_read_compile_errors` | offline | no | Newest `<Project>/Saved/Logs/*.log` tail → structured MSVC/clang diagnostics. The one channel that works when the bridge module itself failed to compile. |
| `unreal_open_mcp_source_read_offline` | offline | no | Offline twin of `source_read` — same `<Project>/Source/` jail, same result shape, same windowing. |
| `unreal_open_mcp_project_index` | offline | no | `.uproject` basics + optional `Source`/`Config`/`Content` file listing (text extensions only). |

### live — discovery & health (1)

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| `unreal_open_mcp_ping` | live | no | Bridge health probe (`GET /ping`). |

### live — gate & validation meta-tools (4)

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| `unreal_open_mcp_validate_edit` | live | no | Scoped health check without a preceding mutation. Bypasses `GatePolicy.Execute` (no recursion). |
| `unreal_open_mcp_checkpoint_create` | live | no | Capture a fingerprint for later delta comparison. |
| `unreal_open_mcp_delta` | live | no | Compare current health vs a stored checkpoint. |
| `unreal_open_mcp_apply_fix` | live | yes | Fix application workflow. Dry-run previews (no mutation, no gate); non-dry-run applies run through the gate runner with a rollback snapshot. |

### live — actor family (7)

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| `unreal_open_mcp_actor_find` | live | no | Read-only actor locator. |
| `unreal_open_mcp_actor_create` | live | yes | Spawn in the current editor level. |
| `unreal_open_mcp_actor_modify` | live | yes | FProperty writes on actor(s). |
| `unreal_open_mcp_object_modify` | live | yes | FProperty writes on any UObject (actor / component / asset instance). |
| `unreal_open_mcp_actor_set_parent` | live | yes | Reparent with a cycle guard. |
| `unreal_open_mcp_actor_duplicate` | live | yes | Spawn-from-template clone. |
| `unreal_open_mcp_actor_destroy` | live | yes | Single + batch actor removal. |

### live — actor component family (5)

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| `unreal_open_mcp_actor_component_add` | live | yes | NewObject + registration. |
| `unreal_open_mcp_actor_component_destroy` | live | yes | Component removal (instance-component gate). |
| `unreal_open_mcp_actor_component_get` | live | no | Read-only component dump. |
| `unreal_open_mcp_actor_component_modify` | live | yes | ApplyProperties on a resolved component. |
| `unreal_open_mcp_actor_component_list_all` | live | no | Read-only components array. |

### live — level family (7)

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| `unreal_open_mcp_level_open` | live | yes | Replace the editor world via `LoadMap` (dirty guard + `ignore_dirty` bypass). |
| `unreal_open_mcp_level_save` | live | yes | Save in place or save-as the persistent level. |
| `unreal_open_mcp_level_list_loaded` | live | no | Read-only persistent + streaming sublevel enumeration. |
| `unreal_open_mcp_level_set_current` | live | yes | Switch the actor-editing context via `MakeLevelCurrent`. |
| `unreal_open_mcp_level_unload_sublevel` | live | yes | Remove a streaming sublevel (persistent-level guard). |
| `unreal_open_mcp_level_get_data` | live | no | Read-only actor roster with profile/pagination + World Partition scope flag. |
| `unreal_open_mcp_level_create` | live | yes | New in-memory or saved-to-disk level, optionally template-seeded. |

### live — asset family (8)

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| `unreal_open_mcp_asset_find` | live | no | Filtered AssetRegistry query, stable ordering + offset/limit pagination. |
| `unreal_open_mcp_asset_get_data` | live | no | Single-asset metadata by path-or-name. |
| `unreal_open_mcp_asset_create_folder` | live | yes | Idempotent `MakeDirectory`; refuses engine roots. |
| `unreal_open_mcp_asset_copy` | live | yes | `DuplicateAsset`; destination must not exist. |
| `unreal_open_mcp_asset_move` | live | yes | `RenameAsset`; a redirector may remain at the source path. |
| `unreal_open_mcp_asset_delete` | live | yes | `DeleteAsset` with a referencer guard (`delete_blocked_by_referencers` unless `force`). |
| `unreal_open_mcp_asset_refresh` | live | no | `ScanPathsSynchronous` — read-only (registry cache only, no on-disk delta). |
| `unreal_open_mcp_asset_import` | live | yes | Absolute host file → `/Game` folder via AssetTools/Interchange. |

### live — material family (3)

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| `unreal_open_mcp_material_create` | live | yes | Create a `UMaterialInstanceConstant` from a parent material interface. |
| `unreal_open_mcp_material_modify` | live | yes | Batch scalar/vector/texture parameter overrides. |
| `unreal_open_mcp_material_get_data` | live | no | Read-only parameter inventory + current values. |

### live — Blueprint family (11)

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| `unreal_open_mcp_blueprint_create` | live | yes | New Blueprint class from a resolvable parent. |
| `unreal_open_mcp_blueprint_get` | live | no | Read-only scoped graph summary for inspection. |
| `unreal_open_mcp_blueprint_add_component` | live | yes | Add an SCS component node (public Simple Construction Script surface). |
| `unreal_open_mcp_blueprint_remove_component` | live | yes | Delete an SCS node (`RemoveNodeAndPromoteChildren`). |
| `unreal_open_mcp_blueprint_add_variable` | live | yes | Add a typed member variable (pin-type forward map). |
| `unreal_open_mcp_blueprint_modify_variable` | live | yes | Rename / retype with validate-before-mutate ordering. |
| `unreal_open_mcp_blueprint_set_default` | live | yes | Write a Class Default Object property (compile-first). |
| `unreal_open_mcp_blueprint_add_function` | live | yes | Create an empty function-graph stub (body authoring out of scope). |
| `unreal_open_mcp_blueprint_add_event` | live | yes | Enable/create an overridable parent event node (body authoring out of scope). |
| `unreal_open_mcp_blueprint_compile` | live | yes | Compile a Blueprint; returns a structured error/warning list (failed compile = data, not a transport error). |
| `unreal_open_mcp_blueprint_spawn` | live | yes | Instance a compiled Actor Blueprint into the current level. |

### live — editor family (4)

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| `unreal_open_mcp_editor_application_get_state` | live | no | Read-only PIE snapshot `{ isPlaying, isPaused, isSimulating, editorMap }`. |
| `unreal_open_mcp_editor_application_set_state` | live | yes | Start/stop/pause/resume driver; all four transitions are latent (return `{ pending:true }`, poll get-state). |
| `unreal_open_mcp_editor_selection_get` | live | no | Read selected actors as identity refs. |
| `unreal_open_mcp_editor_selection_set` | live | yes | Replace-by-refs or explicit `clear`; resolve-before-mutate, refuse-empty. |

### live — console family (3)

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| `unreal_open_mcp_console_get_logs` | live | no | Bounded GLog ring-buffer read with verbosity/category/substring filters. |
| `unreal_open_mcp_console_clear_logs` | live | no | Empty the buffer (buffer-local; read-only). |
| `unreal_open_mcp_console_run_command` | live | yes | `GEngine->Exec`; destructive accepted-risk. |

### live — reflection family (2)

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| `unreal_open_mcp_reflection_method_find` | live | no | UFunction discovery with signature/flag descriptors. |
| `unreal_open_mcp_reflection_method_call` | live | yes | Safety-gated `ProcessEvent` invoke (BlueprintCallable / CallInEditor only). |

### live — screenshot family (4)

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| `unreal_open_mcp_screenshot_viewport` | live | no | `FViewport::ReadPixels`; returns a base64 PNG as MCP image content. |
| `unreal_open_mcp_screenshot_game_view` | live | no | Game-view capture (requires PIE else `pie_not_running`). |
| `unreal_open_mcp_screenshot_camera` | live | no | Render from a resolved camera actor via a transient `ASceneCapture2D`. |
| `unreal_open_mcp_screenshot_isolated` | live | no | Render a single actor against a neutral background, auto-framed by bounds. |

### live — source family (6)

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| `unreal_open_mcp_source_read` | live | no | Read a project C++ file under `<Project>/Source/` (jailed). |
| `unreal_open_mcp_source_list` | live | no | Enumerate source files under `<Project>/Source/` (jailed). |
| `unreal_open_mcp_source_create_class` | live | yes | Scaffold a header + cpp from parent-kind templates (jailed). |
| `unreal_open_mcp_source_update` | live | yes | Full-file replace or 1-based inclusive line-range splice (jailed). |
| `unreal_open_mcp_source_delete` | live | yes | Delete a single file (jailed; destructive, not MCP-undoable). |
| `unreal_open_mcp_source_compile` | live | yes | Live Coding or UnrealBuildTool; returns structured diagnostics (failed compile = data, not a transport error). |

### batch (0 — planned)

| Tool | Route | Mutating | Notes |
|---|---|---|---|
| _(none shipped)_ | batch | — | Headless commandlet dispatch is planned, not implemented. A name classified batch returns `batch_not_implemented`, pointing at the live equivalent or `unreal_open_mcp_capabilities` for the current surface. |

## Offline coverage

When the bridge is dead — the editor crashed, the bridge module failed to
compile (Live Coding failure / a bad C++ edit), or the editor is simply not
running — three offline tools still give an agent useful introspection by
reading the project tree directly from disk. They route `offline` and never
touch the live transport; every result is stamped `_source: "offline"` +
`_route: { route: "offline" }`.

- `unreal_open_mcp_read_compile_errors` — the **one channel that survives a dead
  bridge assembly**. It reads the newest `<Project>/Saved/Logs/*.log` tail and
  parses MSVC + clang diagnostics into the same `{ file, line, severity, message }[]`
  shape `source_compile` returns. The editor writes UBT / Live Coding / compiler
  diagnostics to the log regardless of bridge health. A missing log is a
  non-error `log_not_found`; a clean log is `no_errors_found`.
- `unreal_open_mcp_source_read_offline` — the offline twin of `source_read`:
  same `<Project>/Source/` jail (rejects `..`, absolute-outside, and NTFS
  alternate-data-stream escapes), same result shape, same windowing.
- `unreal_open_mcp_project_index` — parses the `.uproject` descriptor (engine
  association, declared modules, enabled plugins) and optionally lists files
  under an allow-listed root (`Source` / `Config` / `Content`). The listing
  surfaces text extensions only.

There is no persistent parse cache; parsers rebuild per request.

**Explicitly out of scope:** offline reads do **not** parse `.uasset` / `.umap`
binary assets. Binary assets require the live bridge (or, in future, a headless
commandlet). This is an intentional, narrower offline surface than a text-YAML
editor — Unreal assets are binary, so a disk parser cannot reconstruct asset
graphs without the editor. The `bridge_status` `dead_bridge` recovery hint points
at `read_compile_errors` because it is the one diagnostic channel that works when
the bridge assembly itself is dead.

## Mutation and gate contract

Mutating tools require a non-empty `paths_hint` scoped to the project assets they
may touch, and accept a `gate` mode. A live mutation normally runs:

```text
checkpoint → mutate → validate → delta
```

Read `gate.delta`, inline `logs`, and `agentNextSteps` even when the mutation
reports success. Gate modes:

- `enforce` (default for mutating tools) — fail when validation introduces blocking issues;
- `warn` — report issues without blocking;
- `off` — skip the gate when explicitly supported.

`unreal_open_mcp_apply_fix` defaults to `dry_run: true`. Review the preview
before applying. Non-dry-run applies run through the gate runner so a rollback
snapshot protects the asset; a corrupting fix is auto-reverted on failure or on
new errors under `enforce`. Applying a fix with `gate: "off"` commits without
rollback protection; the response carries `rollbackDisabled: true` so the
mutation is visible and the asset health must be verified manually afterward.

The three gate meta-tools (`validate_edit`, `checkpoint_create`, `delta`)
participate in the gate workflow but bypass `GatePolicy.Execute` so they do not
recurse — they surface the explicit checkpoint → mutate → delta contract agents
drive when they want a manual gate pass.

### Failed compile = data, not a transport error

Two compile tools — `unreal_open_mcp_blueprint_compile` and
`unreal_open_mcp_source_compile` — return a structured diagnostic report and
treat a **failed** compile as a normal, expected result, not a transport failure.
The envelope stays `ok:true` and the result carries a populated diagnostics list
(`succeeded:false` for Blueprint, `success:false` + `compile_clean:false` for
C++) so an agent reads the diagnostics, fixes via the structure-edit tools, and
recompiles. Only tool-level errors (missing path / missing asset / malformed
body / `ubt_not_found` / …) map to `ok:false`. This contract is what lets an
agent treat "compile failed" as data rather than an opaque error.

For `source_compile`, `success` (process return 0) is split from `compile_clean`
(zero compiler errors): a loaded editor holds its module DLL, so a UBT relink
fails to write it (`success:false`) even when the compile stage was clean
(`compile_clean:true`). Key off `compile_clean` + the diagnostics, not `success`.

## Discover tools programmatically

Call `unreal_open_mcp_capabilities` first:

```json
{
  "kind": "tools",
  "include_planned": true
}
```

The response is the authoritative current catalog — tool names, schemas,
category, group, route per tool, plus the verify rule and fix catalogs. Prefer it
over hand-maintained totals.

## Source references

- `mcp-server/src/tool-router.ts` — route policy table (source of truth for
  every tool's route) + dispatch spine.
- `mcp-server/src/tools/index.ts` — tool registry (`ALL_TOOLS`).
- `mcp-server/src/capabilities/build-capabilities.ts` — the capability surface
  builder (`unreal_open_mcp_capabilities`).
- `packages/bridge/Source/UnrealOpenMcpEditor/Private/` — live tool handlers + gate wiring.
