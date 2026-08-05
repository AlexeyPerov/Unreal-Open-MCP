# Validation Suite

Standalone desktop app that guides milestone manual validation as repeatable scenario runs across engine MCP toolkits. Targets **Unreal Open MCP** against an Unreal project (e.g. the bundled `demo/` project).

The app replaces long, copy-heavy manual checklists with a guided operator workflow: filesystem + MCP/CLI actions automate setup, MCP-client verification stays human-driven (copy a prompt → run in the agent → paste the result), and progress + evidence persist as project-local data.

This app is a **standalone Tauri app**, not a Hub tab. It ships separately from `hub/` and invokes the engine via the MCP CLI as a subprocess.

## Stack

Tauri 2 + SvelteKit + Svelte 5 (mirrors the Hub frontend). Engine-neutral orchestration is framework-free TypeScript in `packages/core/`.

## Repository layout

```
validation-suite/
  src/                         SvelteKit UI
  src/lib/state/               Svelte 5 runes app state (+ per-step action log)
  src/lib/services/            Tauri IPC wrappers (+ action backend adapter)
  src/lib/components/          UI components (project bar, test nav, step renderer, action log)
  src-tauri/                   Rust shell (sandboxed fs ops, MCP CLI runner, manifests, persistence)
  packages/core/               scenario DTOs, loader, state, action runner, patch transform (engine-neutral TS)
  engine-profiles/
    unreal.json                Unreal profile (paths, CLI, companions, markers)
  scenarios/
    unreal/sample/*.json       shipped sample scenario definitions
```

## Unreal targeting

The `unreal` engine profile (`engine-profiles/unreal.json`) declares how the suite recognizes an Unreal project and where it scopes data:

- **Project markers** — the folder must contain `Content/`, `Source/`, `Config/` directories and at least one `*.uproject` descriptor (glob-matched, so any project name is accepted).
- **Fixture root** — disposable per-scenario fixtures land under `Content/_ValidationSuite/<test-id>/`.
- **State root** — operator state, manifests, actuals, and exports live under `Saved/ValidationSuite/` (Unreal's conventional `Saved/` tree).
- **Tool prefix** — `unreal_open_mcp_` (so prompts reference e.g. `unreal_open_mcp_ping`).
- **CLI binary** — `unreal-open-mcp` (resolved from `PATH`; see [Manual setup](../docs/manual-setup.md) for install/link).
- **Companions** — Unreal has no Unity-style `.meta` companions, so the profile ships an empty companions array. The copy/track machinery is engine-neutral and will honor a companion rule if one is added later.

## Running

```bash
cd validation-suite
npm install
npm run tauri dev
```

Then use **Open project…** to select an Unreal project folder (e.g. `../demo`). The suite validates the folder against the engine profile's project markers and scopes all state + fixtures under that project.

## Core scenarios

The suite ships five guided scenarios under `scenarios/unreal/core/` that exercise the connectivity floor, a mutator round-trip with the gate, a controlled verify failure, the fix workflow, and a viewport capture:

| Scenario | Tool family | Tier |
|---|---|---|
| `core-ping` — bridge ping connectivity smoke | ping | required-core |
| `core-actor-create` — spawn + destroy a PointLight with `gate: enforce` | actor | required-core |
| `core-gate-fail` — `validate_edit` surfaces a staged `broken_soft_reference` | gate & verify | required-core |
| `core-fix` — `apply_fix` dry-run preview then clear the broken soft reference | gate & verify | required-core |
| `core-screenshot` — viewport PNG capture | screenshot | optional |

Use the **Required · core** filter to isolate the four closeout-gate scenarios. `core-screenshot` is `optional` because it needs an interactive GPU session — on a headless / CI host the capture fails and that is a documented skip, not a regression.

### Operator prerequisites

- **Editor open** on the target project (e.g. `demo/`) with both Open MCP plugins compiled — wait for the status bar to settle.
- **MCP client** pointed at the project: set `UNREAL_PROJECT_PATH` to the absolute project path so the server discovers the bridge's deterministic per-project port (see [Manual setup](../docs/manual-setup.md)).
- **Tool groups** — the gate-fail and fix scenarios stage a disposable Blueprint, so the typed-editor group (asset + Blueprint families) must be visible. Activate it via `unreal_open_mcp_manage_tools` if a staging tool reports not found.
- **Deterministic staging** — the gate-fail and fix scenarios create a disposable Blueprint under `/Game/_ValidationSuite/GateFail/`, add a soft-object-pointer variable, and point its default at a deliberately-missing path. The suite stages and reverts this fixture; no demo asset is modified. The fixture lives under the gitignored `Content/_ValidationSuite/` root.
- **Issue id hand-off** — copy the `issue_id` from `core-gate-fail`'s / `core-fix`'s `validate_edit` result; the `apply_fix` steps paste it verbatim.

## Tests

```bash
# Engine-neutral core (node:test, TS)
npm run test:core

# UI type-check
npm run check

# Rust backend
cd src-tauri && cargo test
```

## Setup actions and reset

Scenario `setup` steps run **declarative actions** through an engine-neutral runner (`packages/core/src/actions.ts`) that delegates to a Rust backend. Every fs action is **sandboxed to the project root** — traversal outside the project is rejected.

| Action | Executor | Behavior |
|---|---|---|
| `fs_copy` | Rust | Copies a file or directory tree; auto-tracks a declared companion when the source companion exists. |
| `fs_patch` | Rust | Applies the pinned patch-op vocabulary (`replace_line_contains`, `insert_after_line_contains`, `insert_before_line_contains`, `trim_trailing_whitespace`); snapshots the pre-patch file for reset. |
| `fs_delete` | Rust | Deletes manifest-listed paths (used by reset; no heuristic deletes). |
| `mcp_tool` | Rust subprocess | Runs an MCP tool via the engine CLI; surfaces `isError` and the tool body in the action log. |
| `manual` | UI gate | Records an info log; the operator confirms the action. |

Patch ops are validated at scenario-load time, so an unknown op never reaches the executor. Each mutating step records a **manifest** (created/modified artifacts + snapshots) under `Saved/ValidationSuite/manifests/`; the state file keeps only the blob id per step.

**Reset** walks a step's manifest in reverse order: modified files restore from their snapshot, created artifacts are deleted. Missing/incomplete manifest metadata warns and continues (best-effort) rather than crashing. Run setup from a step's **Run setup** button; re-run or reset with **Re-run setup** / the test-level **Reset test**.

## Where data lives

Per active project + `unreal` profile:

- **State file:** `Saved/ValidationSuite/.state.json` — atomic read/write; survives app restart.
- **Manifests:** `Saved/ValidationSuite/manifests/` — per-step artifact manifests for reset.
- **Actuals:** `Saved/ValidationSuite/actuals/`.
- **Exports:** `Saved/ValidationSuite/exports/` — run-summary markdown.
- **Fixtures:** `Content/_ValidationSuite/<test-id>/`.

State is **not migrated** between versions: a version mismatch produces a warning with reset guidance.

### Demo project hygiene

The suite stages disposable fixtures under `Content/_ValidationSuite/` and writes local state under `Saved/ValidationSuite/`. The bundled `demo/` project's `.gitignore` ignores both so they never get committed:

- `Content/_ValidationSuite/` — staged and reverted per scenario by the suite.
- `Saved/ValidationSuite/` — operator state, manifests, actuals, exports.

## Requirement tiers and optional scenarios

Every scenario declares a `requirementLevel`:

- **required-core** — the milestone closeout gate. Use the **Required · core** filter to isolate these.
- **required-extended** — a recommended confidence pass.
- **optional** — runnable; usually shows automated coverage. Collapsed into a default-closed "Optional" subsection within each milestone group, with an **Auto** badge when `automatedCoverage` references exist.

Optional scenarios carry an `automatedCoverage` array naming the tests that already cover the behavior; they stay runnable so an operator can do a live confidence pass even when automation exists.

## Export (run summary)

Use **Export…** in the top bar to produce a sign-off markdown summary of the current run:

- **Copy summary to clipboard** — markdown ready to paste into a milestone checklist or changelog.
- **Save summary as file…** — writes a timestamped `.md` under `Saved/ValidationSuite/exports/`.

The summary includes the project path, engine profile id, timestamp, a requirement-tier breakdown, one status table per milestone (required grouped, optional folded under an "Optional" subheading), and the closeout-gate verdict (passes only when every `required-core` scenario is `done`). The builder is engine-neutral (`packages/core/src/export.ts`) and unit-tested.
