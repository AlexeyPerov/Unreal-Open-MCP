// CompileErrorsIssueMapper implementation. See header for the per-finding
// shape and the coarse-failure fallback rationale.
#include "Rules/CompileErrors/CompileErrorsIssueMapper.h"

#include "Rules/CompileErrors/CompileErrorsIssueCodes.h"

namespace UnrealOpenMcpVerify::CompileErrors
{

namespace
{

// Pick the AssetPath for a finding: the diagnostic's source file when
// available, otherwise the "(project)" sentinel so the FIssueKey stays
// well-formed (FIssueKey::Build rejects an empty AssetPath).
FString ResolveAssetPath(const FCompileDiagnostic& D)
{
	if (!D.File.IsEmpty())
	{
		return D.File;
	}
	return FString(ProjectAssetPath);
}

// Build the IssueCode suffix "<file>:<line>" when the diagnostic carries
// enough to identify a specific location, otherwise fall back to a
// classifier so the issue key stays unique. The bare code
// ("compile_error") is what the explainability table and the
// FixProviderRegistry key on.
FString BuildSuffix(const FCompileDiagnostic& D)
{
	if (!D.File.IsEmpty() && D.Line > 0)
	{
		return FString::Printf(TEXT("%s:%d"), *D.File, D.Line);
	}
	if (!D.File.IsEmpty())
	{
		return D.File;
	}
	return TEXT("project");
}

// Helper that converts an int32 to its FString, returning empty for 0 so
// the Evidence map only carries line/column when the provider had them.
FString OptInt(int32 Value)
{
	if (Value <= 0)
	{
		return FString();
	}
	return FString::FromInt(Value);
}

} // namespace

void MapDiagnosticToIssue(const FCompileDiagnostic& Diagnostic, TArray<FVerifyIssue>& OutIssues)
{
	const FString AssetPath = ResolveAssetPath(Diagnostic);
	const FString Suffix = BuildSuffix(Diagnostic);
	const FString Code = FString::Printf(TEXT("%s:%s"), IssueCode, *Suffix);

	TMap<FString, FString> Evidence;
	Evidence.Add(TEXT("file"), Diagnostic.File);
	Evidence.Add(TEXT("line"), OptInt(Diagnostic.Line));
	Evidence.Add(TEXT("column"), OptInt(Diagnostic.Column));
	Evidence.Add(TEXT("message"), Diagnostic.Message);
	Evidence.Add(TEXT("module"), Diagnostic.Module);
	Evidence.Add(TEXT("source"), DiagnosticSourcePerFile);

	// Description wording reads naturally for both per-file and coarse-
	// file findings: when the diagnostic has a File, mention it; when it
	// does not, fall back to a "project compile" wording.
	const FString Description = !Diagnostic.File.IsEmpty()
		? FString::Printf(
			TEXT("Compile error in '%s' (line %d): %s"),
			*Diagnostic.File,
			Diagnostic.Line,
			*Diagnostic.Message)
		: FString::Printf(
			TEXT("Project compile failure: %s"),
			*Diagnostic.Message);

	OutIssues.Emplace(
		FString(RuleId),
		EVerifySeverity::Error,
		AssetPath,
		Code,
		Description,
		MoveTemp(Evidence));
}

void MapCoarseFailureToIssue(TArray<FVerifyIssue>& OutIssues)
{
	const FString Code = FString::Printf(TEXT("%s:%s"), IssueCode, TEXT("project"));

	TMap<FString, FString> Evidence;
	Evidence.Add(TEXT("file"), FString());
	Evidence.Add(TEXT("line"), FString());
	Evidence.Add(TEXT("column"), FString());
	Evidence.Add(TEXT("message"), TEXT("Live Coding reported a compile failure with no per-file breakdown"));
	Evidence.Add(TEXT("module"), FString());
	Evidence.Add(TEXT("source"), DiagnosticSourceCoarse);

	const FString Description = TEXT(
		"Project compile failure: Live Coding reported a failure with no per-file breakdown. "
		"Open the Live Coding / Output Log panel for the structured diagnostics.");

	OutIssues.Emplace(
		FString(RuleId),
		EVerifySeverity::Error,
		FString(ProjectAssetPath),
		Code,
		Description,
		MoveTemp(Evidence));
}

} // namespace UnrealOpenMcpVerify::CompileErrors
