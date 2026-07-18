# Verify package rules

## Scope

Rules for `packages/verify/` — the scoped health-check module for Unreal Open MCP. Inherits root `AGENTS.md`; deeper rules win on overlap.

## Package shape

- Unreal Engine 5.6+ C++ Editor module. Editor-only code under `Source/UnrealOpenMcpVerify/`.
- No dependency on the bridge module — verify must stay usable standalone (the bridge depends on verify, not the reverse). The `UnrealOpenMcpVerify.Build.cs` deliberately lists **no** `UnrealOpenMcp*` dependencies; the bridge will take a dependency on this module in P3.5 when the gate flow is wired.
- Tests live under `Source/UnrealOpenMcpVerifyTests/` (Automation specs, `WITH_DEV_AUTOMATION_TESTS`-guarded).
- Module layout:
  - `Public/Core/` — `EVerifySeverity`, `EVerifyRunMode`, `FVerifyScope`, `FVerifyIssue`, `FIssueKey`, `IVerifyRule`, `FVerifyResult`, `FCheckpointFingerprint` (+ `FRuleFingerprint`), `FVerifyRunner`.
  - `Public/Fixes/` — `FFixDescription`, `FFixResult`, `FFixCandidate`, `IFixProvider`, `FFixProviderRegistry`.
  - `Public/Rules/BrokenSoftReferences/` — `FBrokenSoftReferencesRule`, `ISoftPathResolver`, `BrokenSoftReferencesIssueCodes` (P3.2).
  - `Public/Rules/MissingBlueprintParent/` — `FMissingBlueprintParentRule`, `IBlueprintParentResolver`, `MissingBlueprintParentIssueCodes` (P3.3).
  - `Private/Core/`, `Private/Fixes/`, `Private/Rules/BrokenSoftReferences/`, `Private/Rules/MissingBlueprintParent/` — implementations.

## Verify rules

- Every rule implements `IVerifyRule` (plain C++ abstract class — not a UObject) and lives in its own folder under `Rules/{RuleName}/` once concrete families land.
- **Every rule must declare a stable `Id`** (returned by `GetId()`) — surfaced in MCP tool responses, the capability catalog, and the gate delta.
- **Every `FVerifyIssue` must carry an `IssueCode`**. v1 issue codes: `broken_soft_reference` (implemented, P3.2 — `broken_soft_references` rule, walks `FSoftObjectPath` properties and asks `ISoftPathResolver` whether each target resolves), `missing_blueprint_parent` (implemented, P3.3 — `missing_blueprint_parents` rule, reads `UBlueprint::ParentClass` and asks `IBlueprintParentResolver` whether the parent path resolves; null `ParentClass` after a successful `LoadPackage` is reported with `expectedParent="(unknown)"`), `compile_error`, `content_path_hygiene` (still scaffold-only). Issue codes link rules to fixes.
- Severity (`Error` / `Warning`) is set per-issue, not per-rule. The gate delta treats Errors as failures; Warnings are informational.
- `IVerifyRule::Scan` must be side-effect-free beyond appending to the sink — the runner swallows exceptions thrown from Scan() so one bad rule cannot abort a gate pass (WITH_EXCEPTIONS only; in no-exception builds a misbehaving rule surfaces as a crash).

## Issue keys

- `FIssueKey::Build` produces the canonical identity `{RuleId}|{SEVERITY}|{AssetPath}|{IssueCode}` used for gate delta diffs and the apply_fix lookup.
- `FIssueKey::TryParse` is case-insensitive on the severity token and accepts `ERROR` / `WARN` / `WARNING` in any casing so the documented scan → apply_fix loop works across separate calls (regression guard pinned in `specs/feedback.md` 2026-07-03).
- Some issue codes carry a GUID suffix (e.g. `missing_guid:<guid>`) so a fix provider can identify which specific broken reference to rewrite. `BareIssueCode` / `IssueCodeSuffix` strip and extract the suffix.

## Fixes

- Every fix implements `IFixProvider` (plain C++ abstract class) and registers via `FFixProviderRegistry`.
- Every fix must declare a `FixId` (returned by `GetFixId()`) and implement `CanFix(issueId)`.
- `Safe: true` fixes are the only ones the gate will auto-suggest. `FFixProviderRegistry::TryGetFixInfo` / `CandidatesForIssue` surface the provider's REAL Safe flag from `Describe()` (regression guard: a previous Unity implementation hardwired safe=true and masked unsafe fixes as auto-applyable). When `Describe()` throws, the registry defaults Safe to **false** so the gate never auto-applies something it cannot reason about (WITH_EXCEPTIONS only).

## Capability catalog sync

- The MCP-side rule catalog (`mcp-server/src/capabilities/rule-catalog.ts`) mirrors implemented rules and their issue codes/severities. Update the MCP catalog in the same task when rules change so `unreal_open_mcp_capabilities` stays accurate.

## Verification

- C++ changes: add or update the narrowest test in `UnrealOpenMcpVerifyTests/`.
- Rule changes: verify issue → fix linkage in tests and via `build-capabilities.test.ts`.
- The contract specs (IssueKey, VerifyResult/Scope, VerifyRunner, FixProviderRegistry) pin the P3.1 scaffold behavior — every later rule / fix change must keep them green.
