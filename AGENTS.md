# Agent rules

- **Layered AGENTS.md (deepest rule wins).** `AGENTS.md` files are co-located with the code they govern. Precedence flows root → package → subtree: a deeper file may add or narrow rules for its subtree, but never silently contradicts a root rule unless that root rule explicitly allows an exception. On overlap, the deepest applicable rule is most specific. Current layers:
  - Root (`AGENTS.md`) — cross-cutting rules (master-only branching, Unity-first porting, specs gitignored, docs ownership).
  - `packages/bridge/AGENTS.md` — bridge transport, tool registration, gate policy.
  - `packages/verify/AGENTS.md` — verify rules (must declare issue codes), fixes, capability catalog sync.
  - `mcp-server/AGENTS.md` — tool definitions, routing, offline-read no-cache philosophy.
  - `hub/AGENTS.md` — SvelteKit/Tauri UI (deferred).

- **Work on `master`; do not branch.** Unless the user explicitly asks for a separate branch, do all work on `master` and commit there directly. Do not create feature/topic branches (`feat/*`, `fix/*`, etc.) on your own initiative, and do not commit the same logical change across multiple branches. If you inherit a repo that is already on a non-`master` branch, merge it into `master` (fast-forward when possible) and delete the branch before continuing. The single-branch `master` workflow keeps the history linear and avoids drift between branches. Branch only when the user requests it.

- **Mandatory Unity-first porting protocol.** Before planning, documenting, or implementing any non-trivial change (architecture, bridge, MCP tools, gate/verify, CLI, routing, docs that define contracts), consult **Unity Open MCP** as the canonical reference. Do not reinvent patterns that already exist there.

  **Reference order (use the first available):**
  1. Local checkout (preferred): `/Users/alexeyperov/Projects/Unity-AI-Hub`
  2. Public repo (fallback for agents without local access): [https://github.com/AlexeyPerov/Unity-Open-MCP](https://github.com/AlexeyPerov/Unity-Open-MCP)

  **Required workflow:**
  1. Open `specs/porting-map.md` for the current phase (gitignored — local only).
  2. Read the listed Unity Open MCP files and the package `AGENTS.md` for that area.
  3. Read the listed **Unreal behavior reference** files when the porting-map row includes an Unreal reference — use them as **read-only behavior examples** (Unreal editor API usage, handler logic, Blueprint/asset/level edge cases). Never copy third-party SignalR/cloud transport stacks or .NET sidecar MCP hosts from those references. Those projects remain specs-only; do not name them in tracked user-visible docs.
  4. Implement with a fidelity tag per change area: **copy** | **adapt** | **greenfield** | **skip** (see [docs/porting-principles.md](docs/porting-principles.md)).
  5. Record evidence before finishing (see below).

  **Required evidence (every task / PR):**
  - **Unity files consulted** — paths in Unity Open MCP
  - **Unreal behavior reference files consulted** — paths when listed in porting-map (or `—` if none)
  - **Fidelity tags** — per change area
  - **Intentional deltas** — what differs from Unity and why
  - **Test parity note** — Unity test/pattern ported, equivalent Unreal test added, or justified gap
  - **Tool naming** — default `unreal_open_mcp_*` (ADR-003); note only if deliberately aligning with external tool names

  Mirror key evidence in tracked artifacts (PR description using the checklist template). Full detail may also live in `specs/execution/P{n}/execution-plan.md` (gitignored).

- **Specs (`specs/`).** Local working docs only — `specs/` is gitignored. Do not `git add`, commit, or push anything under `specs/`. You may read and edit files there when helpful (e.g. execution plans, backlog, porting map), but keep those changes out of version control.

- **Migrations.** Do not implement data migrations, compatibility shims, or upgrade paths for persisted data unless explicitly requested. Prefer simplifying storage and codecs over backward compatibility.

- **Naming rule exception.** Tracked docs (`README.md`, `docs/`, `AGENTS.md`, UI strings) **may** reference **Unity Open MCP** by name and link to [https://github.com/AlexeyPerov/Unity-Open-MCP](https://github.com/AlexeyPerov/Unity-Open-MCP). Other competitor/reference projects remain **specs-only** — never name them in tracked user-visible surfaces. See `specs/porting-map.md` for the allowed list.

- **No internal references in user-visible surfaces.** User-visible docs and UI strings must never reference internal data such as `specs/` paths, phase IDs (e.g. P0.3, M3), execution-plan task numbers, or porting-map citations. Source-code comments may reference specs for developer context; shipped documentation must be clean.

- **No roadmap / phase-exit framing in tracked docs.** `README.md` and `docs/` describe current behavior and how to verify it — not which roadmap phase shipped a feature or closed a gate. Do not add sections, headings, or bullets like “Phase N parity smoke”, “mandatory gate before Phase N+1”, or “shipped in P2.2”. Smoke and integration coverage belongs under evergreen names (e.g. E2E smoke verification): document how to run the tests and what failure codes mean. Roadmap exit-gate narrative stays in `specs/` (gitignored) or in source/test comments.

- **Docs are part of done.** If a change affects public behavior, API contracts, architecture boundaries, or developer workflows, update tracked docs in `README.md` and/or `docs/` in the same task.

- **Docs layout and ownership.**
  - Root `README.md` stays short: intro, current feature set, quick links, and a **Documentation** section that is the docs index.
  - `docs/architecture.md` covers repo structure and cross-package boundaries.
  - `docs/api.md` documents externally relevant interfaces and contracts.
  - `docs/porting-principles.md` documents the Unity-first porting protocol and fidelity rubric.

- **Doc update scope rule.** Edit only docs that match the changed area; avoid unrelated rewrites.

- **When docs updates can be skipped.** Typos, formatting-only edits, comments-only changes, and internal refactors that do not alter behavior or contracts.

- **Agent reporting requirement.** If code changes but tracked docs are not updated, explicitly state why docs were not needed in the final handoff message. If Unity-first evidence was not recorded, state why the change was trivial enough to skip.

- **Agent MCP-experience feedback loop.** Any agent that uses Unreal Open MCP tools during development must, before finishing its turn/task, scan tool usage for problems and append dated entries to `specs/feedback.md` (create if missing). Format:

  ```
  - **Date:** YYYY-MM-DD
  - **Tool:** unreal_open_mcp_<name>
  - **What happened:** <observed behavior>
  - **Expected:** <correct behavior>
  - **Severity:** bug | friction | suggestion
  - **Suggested fix:** <idea or execution-plan ref>
  ```

  `specs/feedback.md` is gitignored. Do not duplicate entries for the same tool/issue — append `+1 / reproduces on <date>` instead.
