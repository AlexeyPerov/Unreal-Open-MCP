// FMissingBlueprintParentRule implementation. See header for the resolver
// injection rationale.
#include "Rules/MissingBlueprintParent/MissingBlueprintParentRule.h"

#include "Rules/MissingBlueprintParent/AssetRegistryBlueprintParentResolver.h"
#include "Rules/MissingBlueprintParent/MissingBlueprintParentIssueCodes.h"
#include "Rules/MissingBlueprintParent/MissingBlueprintParentScanner.h"

FMissingBlueprintParentRule::FMissingBlueprintParentRule(TUniquePtr<IBlueprintParentResolver> InResolver)
	: Resolver(MoveTemp(InResolver))
{
}

FMissingBlueprintParentRule::FMissingBlueprintParentRule()
	: Resolver(MakeUnique<FAssetRegistryBlueprintParentResolver>())
{
}

FString FMissingBlueprintParentRule::GetId() const
{
	return FString(MissingBlueprintParent::RuleId);
}

void FMissingBlueprintParentRule::Scan(const FVerifyScope& Scope, const EVerifyRunMode Mode, TArray<FVerifyIssue>& Sink) const
{
	if (Scope.Paths.Num() == 0)
	{
		// Whole-project scans are explicitly out of scope for v1 (a full
		// parent-resolution walk over every Blueprint package is expensive
		// and noisy). Return quietly so a Full-mode scan_paths without
		// paths_hint does not emit zero unknown-rule errors but also does not
		// run away. Mirrors the broken_soft_references rule's empty-scope
		// guard.
		return;
	}

	// This rule is cheap (one parent lookup per Blueprint), so it runs in
	// every mode including Checkpoint. Unlike broken_soft_references there is
	// no struct-recursion to defer.
	TArray<FName> LoadedPkgs;
	MissingBlueprintParent::ScanScope(Scope, *Resolver, Sink, LoadedPkgs);
	(void)LoadedPkgs;
}
