// FAssetRegistrySoftPathResolver implementation. See header for the
// resolution order rationale (Asset Registry first, TryLoad fall-back).
//
// Uses GetAssetsByPackageName (stable across UE 5.6+) rather than the
// version-flavored GetAssetPackageData / TryGetAssetPackageData, so the
// resolver compiles without per-engine-version branches.
#include "Rules/BrokenSoftReferences/AssetRegistrySoftPathResolver.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/SoftObjectPath.h"

bool FAssetRegistrySoftPathResolver::Resolve(const FString& SoftPath) const
{
	if (SoftPath.IsEmpty())
	{
		return false;
	}

	// Normalize via FSoftObjectPath so package.asset / package.asset:subobject
	// forms all collapse to a comparable asset path + package name. A path
	// that FSoftObjectPath cannot parse is treated as unresolved (return false)
	// rather than thrown -- the runner swallows exceptions but a clean false
	// keeps the scan quiet in no-exception builds.
	const FSoftObjectPath Normalized(*SoftPath);
	const FName PackageName = Normalized.GetPackageName();

	const FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& Registry = Module.Get();

	// O(1)-ish path: does the Asset Registry know about any asset in this
	// package? GetAssetsByPackageName is stable across UE 5.6+ and avoids the
	// version-flavored GetAssetPackageData / TryGetAssetPackageData split.
	TArray<FAssetData> OutAssets;
	if (Registry.GetAssetsByPackageName(PackageName, OutAssets, /*bIncludeOnlyOnDiskAssets=*/false) && OutAssets.Num() > 0)
	{
		return true;
	}

	// Fall-back: the registry may not have scanned this package yet (e.g. it
	// was saved after the registry's last sweep). TryLoad pulls it in.
	// TryLoad() is cached internally by the soft object path system, so repeat
	// Resolve() calls for the same path do not re-load.
	return Normalized.TryLoad() != nullptr;
}
