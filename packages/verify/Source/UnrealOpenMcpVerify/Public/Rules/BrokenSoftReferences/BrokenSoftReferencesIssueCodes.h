// Stable issue codes for the broken_soft_references rule.
//
// Greenfield (Unity Open MCP's MissingReferences rule walks YAML/GUID refs;
// Unreal uses soft paths and the Asset Registry — different identity model).
// This rule emits a single code today; the constants live here so the rule,
// the scanner, the explainability table (P3.8), and a future Safe fix
// (P3.7) share one source of truth.
//
// Declared `static constexpr` so each translation unit that includes the
// header folds its own copy — no exported symbol, no ODR/linker pain, and
// callers always compare by FString value (GetId() returns FString).
#pragma once

#include "CoreMinimal.h"

namespace UnrealOpenMcpVerify::BrokenSoftReferences
{

// Rule Id surfaced in FVerifyIssue::RuleId / FVerifyRunner registration.
// Pinned in specs/execution/P3/P3.2.md and packages/verify/AGENTS.md.
static constexpr const TCHAR* RuleId = TEXT("broken_soft_references");

// v1 issue code for an unresolved soft object reference. May carry a suffix
// "<package-name>:<property-path>" so a future fix provider can identify
// exactly which soft pointer to rewrite when an asset has several. The bare
// code ("broken_soft_reference") is what the explainability table and the
// FixProviderRegistry key on; the suffix is parsed via FIssueKey helpers.
//
// Identical FVerifyIssue fields for every emitter:
//   RuleId      = broken_soft_references
//   Severity    = Error (unresolved soft target is a real breakage)
//   AssetPath   = the referencing package path (/Game/.../X.uasset)
//   IssueCode   = broken_soft_reference[:<suffix>]
//   Description = human-readable copy
//   Evidence    = { assetPath, softPath, propertyName, targetPackage }
static constexpr const TCHAR* IssueCode = TEXT("broken_soft_reference");

} // namespace UnrealOpenMcpVerify::BrokenSoftReferences
