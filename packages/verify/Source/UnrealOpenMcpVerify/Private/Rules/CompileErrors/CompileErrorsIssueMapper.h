// CompileErrorsIssueMapper - translate provider diagnostics into the
// FVerifyIssue surface, mirroring MissingBlueprintParentIssueMapper /
// BrokenSoftReferencesIssueMapper role.
//
// Greenfield. Each FCompileDiagnostic becomes one FVerifyIssue with:
//   RuleId      = compile_errors
//   Severity    = Error
//   AssetPath   = the diagnostic's source file, or "(project)" when the
//                 diagnostic carries no File (a coarse Live Coding failure)
//   IssueCode   = compile_error[:<file>:<line>]  (suffix lets a future fix
//                 provider pick which diagnostic to act on; the bare code
//                 "compile_error" is what the explainability table and
//                 FixProviderRegistry key on)
//   Description = agent-facing copy with the diagnostic message
//   Evidence    = file, line?, column?, message, module?, source
//
// A "(project)" coarse finding (no per-file breakdown) is also emitted
// when GetState() == Failed but the provider's diagnostics list is empty
// — a known failure must never be silently swallowed.
#pragma once

#include "CoreMinimal.h"

#include "Core/VerifyIssue.h"
#include "Rules/CompileErrors/ICompileStatusProvider.h"

namespace UnrealOpenMcpVerify::CompileErrors
{

// Origin labels stamped into Evidence("source") so an operator can tell
// whether the finding came from a structured per-file diagnostic or from
// the coarse "Live Coding failed with no breakdown" path. Plain constexpr
// constants (no namespace wrapper) — matches the IssueCodes convention.
static constexpr const TCHAR* DiagnosticSourcePerFile = TEXT("structured");
static constexpr const TCHAR* DiagnosticSourceCoarse = TEXT("coarse");

/**
 * Append one FVerifyIssue per diagnostic into OutIssues.
 *
 * @param Diagnostic  the structured diagnostic to map. FCompileDiagnostic
 *                    lives at global scope (mirrors EBlueprintParentResolution
 *                    next to IBlueprintParentResolver).
 * @param OutIssues   sink for compile_error findings.
 */
void MapDiagnosticToIssue(const ::FCompileDiagnostic& Diagnostic, TArray<FVerifyIssue>& OutIssues);

/**
 * Append a single coarse "(project)" compile_error finding into OutIssues.
 * Used when the provider reports a Failed state with no per-file
 * diagnostics — a known Live Coding failure must never be silently
 * swallowed.
 *
 * @param OutIssues   sink for the coarse compile_error finding.
 */
void MapCoarseFailureToIssue(TArray<FVerifyIssue>& OutIssues);

} // namespace UnrealOpenMcpVerify::CompileErrors
