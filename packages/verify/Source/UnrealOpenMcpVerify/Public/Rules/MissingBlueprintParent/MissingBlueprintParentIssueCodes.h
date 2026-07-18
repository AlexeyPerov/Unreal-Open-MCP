// Stable issue codes for the missing_blueprint_parents rule.
//
// Greenfield. Unity Open MCP has no direct analogue (its missing_references
// rule walks MonoBehaviour script slots in YAML/GUID form, not class
// inheritance). The concept-of-record is "Unreal's analogue of Unity/Godot
// 'missing script'": a Blueprint whose parent class no longer resolves.
//
// This rule emits a single code today; the constants live here so the rule,
// the scanner, the explainability table (P3.8), and a future Safe fix
// (P3.7 — likely skipped for this code; clearing a missing parent is rarely
// Safe) share one source of truth.
//
// Declared `static constexpr` so each translation unit that includes the
// header folds its own copy — no exported symbol, no ODR/linker pain, and
// callers always compare by FString value (GetId() returns FString).
#pragma once

#include "CoreMinimal.h"

namespace UnrealOpenMcpVerify::MissingBlueprintParent
{

// Rule Id surfaced in FVerifyIssue::RuleId / FVerifyRunner registration.
// Pinned in specs/execution/P3/P3.3.md and packages/verify/AGENTS.md.
//
// Plural ("missing_blueprint_parents") to mirror Unity's rule-naming
// convention (e.g. "missing_references"); the issue code is singular
// ("missing_blueprint_parent") so a single finding reads naturally.
static constexpr const TCHAR* RuleId = TEXT("missing_blueprint_parents");

// v1 issue code for a Blueprint whose parent class fails to resolve. May
// carry a suffix "<expected-parent-path>" so a future fix provider can
// identify exactly which parent to repair when an asset has multiple broken
// parents in scope (today: one per Blueprint, but the contract is suffix-
// ready). The bare code ("missing_blueprint_parent") is what the
// explainability table and FixProviderRegistry key on; the suffix is parsed
// via FIssueKey helpers.
//
// Identical FVerifyIssue fields for every emitter:
//   RuleId      = missing_blueprint_parents
//   Severity    = Error (an unresolved parent breaks the Blueprint hierarchy)
//   AssetPath   = the Blueprint asset path (/Game/.../BP_X.BP_X)
//   IssueCode   = missing_blueprint_parent[:<expected-parent>]
//   Description = human-readable copy
//   Evidence    = assetPath, expectedParent, reason
static constexpr const TCHAR* IssueCode = TEXT("missing_blueprint_parent");

} // namespace UnrealOpenMcpVerify::MissingBlueprintParent
