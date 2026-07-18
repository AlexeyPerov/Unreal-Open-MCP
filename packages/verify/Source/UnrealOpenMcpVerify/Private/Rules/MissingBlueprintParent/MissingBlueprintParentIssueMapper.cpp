// MissingBlueprintParentIssueMapper implementation. See header.
#include "Rules/MissingBlueprintParent/MissingBlueprintParentIssueMapper.h"

#include "Rules/MissingBlueprintParent/MissingBlueprintParentIssueCodes.h"

namespace UnrealOpenMcpVerify::MissingBlueprintParent
{

namespace
{

// Build the IssueCode suffix. The suffix is the expected-parent path so a
// future fix provider can identify exactly which parent to repair (today one
// per asset, but the contract is suffix-ready). The bare code
// ("missing_blueprint_parent") is what the explainability table and the
// FixProviderRegistry key on.
FString BuildSuffix(const FMissingBlueprintParentFinding& F)
{
	if (F.ExpectedParent.IsEmpty() || F.ExpectedParent == TEXT("(unknown)"))
	{
		// No usable parent path — fall back to a classifier suffix so the
		// issue key stays unique per asset (the FIssueKey already
		// distinguishes by AssetPath; the suffix just carries the human
		// signal).
		switch (F.Outcome)
		{
			case EBlueprintParentResolution::MissingBlueprintPackage:
				return TEXT("unknown_package");
			case EBlueprintParentResolution::MissingNative:
			case EBlueprintParentResolution::Resolved:
			default:
				return TEXT("unknown_native");
		}
	}
	return F.ExpectedParent;
}

// Pick the description prefix based on the resolution outcome so an operator
// sees the failure mode at a glance.
const TCHAR* OutcomePrefix(EBlueprintParentResolution Outcome)
{
	switch (Outcome)
	{
		case EBlueprintParentResolution::MissingBlueprintPackage:
			return TEXT("Missing Blueprint parent package");
		case EBlueprintParentResolution::MissingNative:
			return TEXT("Missing native parent class");
		case EBlueprintParentResolution::Resolved:
		default:
			return TEXT("Unresolved parent class");
	}
}

} // namespace

void MapFindingToIssue(const FMissingBlueprintParentFinding& Finding, TArray<FVerifyIssue>& OutIssues)
{
	const FString Suffix = BuildSuffix(Finding);
	const FString Code = FString::Printf(TEXT("%s:%s"), IssueCode, *Suffix);

	TMap<FString, FString> Evidence;
	Evidence.Add(TEXT("assetPath"), Finding.AssetPath);
	Evidence.Add(TEXT("expectedParent"), Finding.ExpectedParent);
	Evidence.Add(TEXT("reason"), Finding.Reason);

	const FString Description = FString::Printf(
		TEXT("%s: '%s' on Blueprint '%s' does not resolve (%s)."),
		OutcomePrefix(Finding.Outcome),
		*Finding.ExpectedParent,
		*Finding.AssetPath,
		*Finding.Reason);

	OutIssues.Emplace(
		FString(RuleId),
		EVerifySeverity::Error,
		Finding.AssetPath,
		Code,
		Description,
		MoveTemp(Evidence));
}

} // namespace UnrealOpenMcpVerify::MissingBlueprintParent
