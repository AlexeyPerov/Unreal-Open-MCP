# Verify package rules

## Scope

Rules for `packages/verify/` — the scoped health-check module for Unreal Open MCP. Inherits root `AGENTS.md`; deeper rules win on overlap.

## Package shape

- Unreal Engine 5.6+ C++ Editor module. Editor-only code under `Source/UnrealOpenMcpVerify/`.
- No dependency on the bridge module — verify must stay usable standalone (the bridge depends on verify, not the reverse).
- Tests live under `Source/UnrealOpenMcpVerifyTests/` (Automation specs).

## Verify rules

- Every rule implements `IVerifyRule` and lives in its own folder under `Rules/{RuleName}/`.
- **Every rule must declare a stable `Id`** — surfaced in MCP tool responses, the capability catalog, and the gate delta.
- **Every `VerifyIssue` must carry an `IssueCode`**. v1 issue codes (sketch): `broken_soft_reference`, `missing_blueprint_parent`, `compile_error`, `content_path_hygiene`. Issue codes link rules to fixes.
- Severity (`Error` / `Warning`) is set per-issue, not per-rule. The gate delta treats Errors as failures; Warnings are informational.

## Fixes

- Every fix implements `IFixProvider` and registers via `FixProviderRegistry`.
- Every fix must declare a `FixId` and implement `CanFix(issueId)`.
- `Safe: true` fixes are the only ones the gate will auto-suggest.

## Capability catalog sync

- The MCP-side rule catalog (`mcp-server/src/capabilities/rule-catalog.ts`) mirrors implemented rules and their issue codes/severities. Update the MCP catalog in the same task when rules change so `unreal_open_mcp_capabilities` stays accurate.

## Verification

- C++ changes: add or update the narrowest test in `UnrealOpenMcpVerifyTests/`.
- Rule changes: verify issue → fix linkage in tests and via `build-capabilities.test.ts`.
