// FBrokenSoftReferencesRule implementation. See header for the resolver
// injection rationale.
#include "Rules/BrokenSoftReferences/BrokenSoftReferencesRule.h"

#include "Rules/BrokenSoftReferences/AssetRegistrySoftPathResolver.h"
#include "Rules/BrokenSoftReferences/BrokenSoftReferencesIssueCodes.h"
#include "Rules/BrokenSoftReferences/BrokenSoftReferencesScanner.h"

FBrokenSoftReferencesRule::FBrokenSoftReferencesRule(TUniquePtr<ISoftPathResolver> InResolver)
	: Resolver(MoveTemp(InResolver))
{
}

FBrokenSoftReferencesRule::FBrokenSoftReferencesRule()
	: Resolver(MakeUnique<FAssetRegistrySoftPathResolver>())
{
}

FString FBrokenSoftReferencesRule::GetId() const
{
	return FString(BrokenSoftReferences::RuleId);
}

void FBrokenSoftReferencesRule::Scan(const FVerifyScope& Scope, const EVerifyRunMode Mode, TArray<FVerifyIssue>& Sink) const
{
	if (Scope.Paths.Num() == 0)
	{
		// Whole-project scans are explicitly out of scope for v1 (a full soft
		// ref walk over every package is expensive and noisy). Return quietly
		// so a Full-mode scan_paths without paths_hint does not emit zero
		// unknown-rule errors but also does not run away.
		return;
	}

	const bool bFullScan = Mode != EVerifyRunMode::Checkpoint;

	// LoadedPkgs is collected by the scanner so callers (tests, future gate
	// memory bookkeeping) can observe what the scan pulled in. Unreal does
	// not expose a clean single-package unload API -- ClosePackages /
	// LoadPackage reload reclaims memory lazily via the next GC sweep, so we
	// intentionally leave the packages resident here. A future task may add
	// explicit eviction if a measured working-set regression justifies it.
	TArray<FName> LoadedPkgs;
	BrokenSoftReferences::ScanScope(Scope, bFullScan, *Resolver, Sink, LoadedPkgs);
	(void)LoadedPkgs;
}
