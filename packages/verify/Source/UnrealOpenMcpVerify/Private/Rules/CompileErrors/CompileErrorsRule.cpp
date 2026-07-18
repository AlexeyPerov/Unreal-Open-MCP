// FCompileErrorsRule implementation. See header for the provider-injection
// rationale and the paths_hint handling.
#include "Rules/CompileErrors/CompileErrorsRule.h"

#include "Rules/CompileErrors/CompileErrorsIssueMapper.h"
#include "Rules/CompileErrors/CompileErrorsIssueCodes.h"
#include "Rules/CompileErrors/LiveCodingCompileStatusProvider.h"

namespace UnrealOpenMcpVerify::CompileErrors
{

namespace
{

// True when a diagnostic's File is "in scope" for the supplied paths_hint.
// Compile failures are project-wide state, but a scoped Validate pass
// (non-empty paths_hint) wants to bound which per-file findings it sees —
// mirroring how the broken_soft_references and missing_blueprint_parents
// rules bound their walks to paths_hint.
//
// Matching is deliberately prefix-loose so the same hint forms work as in
// the other rules:
//   - "/Game/Foo/Bar.uasset"  — content path (unusual for source, but the
//     matcher still handles it; a C++ source File is typically
//     "Source/Module/Foo.cpp" or an absolute engine path)
//   - "Source/Module"          — matches every file under the module
//   - "Source/Module/Foo.cpp"  — matches one file
// An empty diagnostic File always matches (the coarse "(project)" finding
// must remain visible even under a scoped hint — a known failure must
// never be silently swallowed).
bool IsDiagnosticInScope(const FString& DiagnosticFile, const TArray<FString>& PathsHint)
{
	if (DiagnosticFile.IsEmpty())
	{
		return true;
	}
	if (PathsHint.Num() == 0)
	{
		return true; // whole-project Full scan — no filtering
	}
	for (const FString& Hint : PathsHint)
	{
		if (Hint.IsEmpty())
		{
			continue;
		}
		// Case-insensitive prefix match against the hint. C++ source paths
		// come in several forms ("Source/Mod/Foo.cpp", absolute under the
		// project dir, sometimes engine-relative); the prefix match keeps
		// the matcher forgiving without inventing path-normalization that
		// would drift across UE versions.
		if (DiagnosticFile.StartsWith(Hint, ESearchCase::IgnoreCase))
		{
			return true;
		}
		// Also accept the reverse direction (the hint is longer than the
		// file path because the hint points at a specific .cpp while the
		// diagnostic only carried a directory) — uncommon, but cheap.
		if (Hint.StartsWith(DiagnosticFile, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

} // namespace

} // namespace UnrealOpenMcpVerify::CompileErrors

FCompileErrorsRule::FCompileErrorsRule(TUniquePtr<ICompileStatusProvider> InProvider)
	: Provider(MoveTemp(InProvider))
{
}

FCompileErrorsRule::FCompileErrorsRule()
	: Provider(MakeUnique<UnrealOpenMcpVerify::CompileErrors::FLiveCodingCompileStatusProvider>())
{
}

FString FCompileErrorsRule::GetId() const
{
	return FString(UnrealOpenMcpVerify::CompileErrors::RuleId);
}

void FCompileErrorsRule::Scan(const FVerifyScope& Scope, const EVerifyRunMode Mode, TArray<FVerifyIssue>& Sink) const
{
	if (!Provider.IsValid())
	{
		// Defensive: a rule constructed via the default path always has a
		// provider, but a misuse that hands in a null unique ptr must not
		// crash the editor. Emit nothing and let the runner continue.
		return;
	}

	// ECompileState / FCompileDiagnostic live at global scope alongside
	// ICompileStatusProvider (mirrors EBlueprintParentResolution next to
	// IBlueprintParentResolver). The rule's helpers (IsDiagnosticInScope,
	// MapDiagnosticToIssue, MapCoarseFailureToIssue) live in the
	// UnrealOpenMcpVerify::CompileErrors namespace and are qualified below.
	const ECompileState State = Provider->GetState();

	// Clean OR InProgressOrUnknown: emit nothing. A clean project has no
	// findings; a compile in flight has no stable result yet (a finding
	// emitted now would be stale before it reached the agent).
	if (State != ECompileState::Failed)
	{
		return;
	}

	TArray<FCompileDiagnostic> Diagnostics;
	Provider->GetDiagnostics(Diagnostics);

	// Filter per-file diagnostics by paths_hint when one is supplied. The
	// coarse "(project)" finding (emitted below when the filtered list is
	// empty) is NOT subject to this filter — see IsDiagnosticInScope's
	// empty-File branch.
	TArray<FCompileDiagnostic> InScope;
	for (const FCompileDiagnostic& D : Diagnostics)
	{
		if (UnrealOpenMcpVerify::CompileErrors::IsDiagnosticInScope(D.File, Scope.Paths))
		{
			InScope.Add(D);
		}
	}

	if (InScope.Num() > 0)
	{
		for (const FCompileDiagnostic& D : InScope)
		{
			UnrealOpenMcpVerify::CompileErrors::MapDiagnosticToIssue(D, Sink);
		}
		return;
	}

	// Provider reported Failed but no per-file diagnostics survived the
	// paths_hint filter (or the provider never had any). Emit a single
	// coarse "(project)" finding so a known failure is never silently
	// swallowed. This is the path the v1 production provider takes
	// whenever it would report Failed.
	UnrealOpenMcpVerify::CompileErrors::MapCoarseFailureToIssue(Sink);
}
