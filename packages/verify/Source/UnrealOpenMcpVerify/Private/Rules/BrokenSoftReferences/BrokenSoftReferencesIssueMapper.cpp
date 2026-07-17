// BrokenSoftReferencesIssueMapper implementation. See header.
#include "Rules/BrokenSoftReferences/BrokenSoftReferencesIssueMapper.h"

#include "Rules/BrokenSoftReferences/BrokenSoftReferencesIssueCodes.h"

namespace UnrealOpenMcpVerify::BrokenSoftReferences
{

// Build the IssueCode suffix. The suffix is "<targetPackage>:<property>"
// so a future fix provider can identify exactly which soft pointer to
// rewrite when an asset has several pointing at the same target. Empty
// property paths degrade to "<targetPackage>".
FString BuildSuffix(const FBrokenSoftReferenceFinding& F)
{
	FString TargetPackage = F.SoftPath;
	// Strip the asset tail so the suffix is a package name, not a full asset
	// path -- keeps the suffix short and stable across reloads.
	const int32 Dot = TargetPackage.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	if (Dot != INDEX_NONE)
	{
		TargetPackage = TargetPackage.Left(Dot);
	}

	if (F.PropertyPath.IsEmpty())
	{
		return TargetPackage;
	}
	return FString::Printf(TEXT("%s:%s"), *TargetPackage, *F.PropertyPath);
}

void MapFindingsToIssues(const TArray<FBrokenSoftReferenceFinding>& Findings, TArray<FVerifyIssue>& OutIssues)
{
	for (const FBrokenSoftReferenceFinding& F : Findings)
	{
		const FString Suffix = BuildSuffix(F);
		const FString Code = FString::Printf(TEXT("%s:%s"), IssueCode, *Suffix);

		TMap<FString, FString> Evidence;
		Evidence.Add(TEXT("assetPath"), F.ReferencingAsset);
		Evidence.Add(TEXT("softPath"), F.SoftPath);
		Evidence.Add(TEXT("propertyName"), F.PropertyPath.IsEmpty() ? TEXT("(unknown)") : F.PropertyPath);

		const FString PropertyLabel = F.PropertyPath.IsEmpty() ? TEXT("(unknown property)") : F.PropertyPath;
		const FString Description = FString::Printf(
			TEXT("Broken soft object reference: '%s' on '%s' does not resolve to a loadable asset."),
			*F.SoftPath, *PropertyLabel);

		OutIssues.Emplace(
			FString(RuleId),
			EVerifySeverity::Error,
			F.ReferencingAsset,
			Code,
			Description,
			MoveTemp(Evidence));
	}
}

} // namespace UnrealOpenMcpVerify::BrokenSoftReferences
