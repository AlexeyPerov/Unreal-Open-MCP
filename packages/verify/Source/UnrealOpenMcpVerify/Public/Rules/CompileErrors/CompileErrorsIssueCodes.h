// Stable issue codes for the compile_errors rule.
//
// Greenfield. Unity Open MCP has no direct equivalent verify rule: Unity's
// compile wait lives in the bridge transport (a "compile-then-continue"
// behavior), not as a verify rule emitting a stable issue code. Unreal's
// analogue needs an explicit rule because Live Coding can leave the editor
// in a failed-compile state after a hot-reload churn, and the gate delta
// needs a stable code to detect it.
//
// This rule emits a single code today; the constants live here so the rule,
// the status provider, the explainability table (P3.8), and a future Safe
// fix (P3.7 — likely skipped; a compile error is an author-side defect the
// agent must edit source to clear, not a Safe automated rewrite) share one
// source of truth.
//
// Declared `static constexpr` so each translation unit that includes the
// header folds its own copy — no exported symbol, no ODR/linker pain, and
// callers always compare by FString value (GetId() returns FString).
#pragma once

#include "CoreMinimal.h"

namespace UnrealOpenMcpVerify::CompileErrors
{

// Rule Id surfaced in FVerifyIssue::RuleId / FVerifyRunner registration.
// Pinned in specs/execution/P3/P3.4.md and packages/verify/AGENTS.md.
//
// Plural ("compile_errors") to mirror Unity's rule-naming convention
// (e.g. "missing_references"); the issue code is singular
// ("compile_error") so a single finding reads naturally.
static constexpr const TCHAR* RuleId = TEXT("compile_errors");

// v1 issue code for an active C++ / Live Coding compile failure. May carry
// a suffix "<file>:<line>" so a future fix provider / the apply_fix flow
// can identify which specific diagnostic to act on when multiple compile
// errors are present in scope. The bare code ("compile_error") is what the
// explainability table and the FixProviderRegistry key on; the suffix is
// parsed via FIssueKey helpers.
//
// Identical FVerifyIssue fields for every emitter:
//   RuleId      = compile_errors
//   Severity    = Error (a failed compile blocks the editor's hot-reload)
//   AssetPath   = the source file path the diagnostic pinned, or the
//                 project sentinel "(project)" when the rule has no file
//                 (a coarse "Live Coding reported failure" finding)
//   IssueCode   = compile_error[:<file>:<line>]
//   Description = human-readable copy with the diagnostic message
//   Evidence    = file, line?, column?, message, module?, source
//
// Note on AssetPath: FIssueKey requires a non-empty AssetPath, so the rule
// stamps the affected source file when one is available and falls back to
// the "(project)" sentinel for a coarse Live Coding failure that has no
// per-diagnostic breakdown (Live Coding surfaces only an enum result, not
// rows). The sentinel keeps the issue keyable into the gate delta without
// inventing a file path.
static constexpr const TCHAR* IssueCode = TEXT("compile_error");

// Sentinel AssetPath for a project-wide compile failure with no per-file
// diagnostic (e.g. Live Coding's coarse Failure enum). Keeps the issue
// keyable into FIssueKey without inventing a source file path.
static constexpr const TCHAR* ProjectAssetPath = TEXT("(project)");

} // namespace UnrealOpenMcpVerify::CompileErrors
