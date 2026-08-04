# Validation Suite rules

## Scope

Rules for `validation-suite/` — standalone guided manual-validation app.
Root `AGENTS.md` also applies. Overview: [`README.md`](README.md).

## Not the Hub

- Same Tauri + SvelteKit stack as `hub/`, but a **separate product**. Do not
  share Hub state, commands, or UI modules. Engine-neutral orchestration lives
  in `packages/core/`; engine specifics in `engine-profiles/` and scenario JSON.

## Unreal engine profile

- The bundled profile is `unreal` (`engine-profiles/unreal.json`). It targets
  an Unreal C++ project: markers `Content/` + `Source/` + `Config/` and a
  `*.uproject` descriptor; fixtures under `Content/_ValidationSuite/`; state
  under `Saved/ValidationSuite/`; tool prefix `unreal_open_mcp_`.
- Unreal ships **no** Unity-style `.meta` companions — leave the profile's
  `companions` array empty. Do not invent fake companion rules.
- The MCP tool name in scenarios/prompts is the full prefixed name
  (e.g. `unreal_open_mcp_ping`), never the bare tool name.

## Scenario conventions

- Hand-author scenarios as JSON under `scenarios/<engineId>/<pack>/`.
- Required fields: `id`, `title`, `milestone`, `engineId`, `requirementLevel`
  (`required-core` | `required-extended` | `optional`), and `steps`.
- Prefer stable ids like `<pack>-<slug>` matching the filename stem.
  Step ids are unique within a scenario. Patch ops are the pinned vocabulary
  validated by `packages/core` (`replace_line_contains`,
  `insert_after_line_contains`, `insert_before_line_contains`,
  `trim_trailing_whitespace`).
- Loader/types in `packages/core/src/` are canonical; reject unknown step or
  action types rather than silently ignoring them.

## Artifacts

- **Hand-authored:** scenario JSON, engine profiles, UI/Rust source.
- **Generated at runtime (do not commit):** project-local suite state under
  `Saved/ValidationSuite/`, fixtures under `Content/_ValidationSuite/`.
  Demo `.gitignore` already excludes both.
- Export summaries are operator artifacts, not repo sources.

## Checklist integration

- Suite export/sign-off can index maintainer milestone checklists when
  `specs/` is available. Public clones are not blocked by missing specs.
- Do not put milestone/spec identifiers into Hub or user-facing product docs.

## Verification

- `npm run test:core`, `npm run check`, and `cargo test` in `src-tauri/` after
  core/UI/Rust changes.
