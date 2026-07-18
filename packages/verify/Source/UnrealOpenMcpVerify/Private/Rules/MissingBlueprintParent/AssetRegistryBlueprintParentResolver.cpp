// FAssetRegistryBlueprintParentResolver implementation. See header for the
// resolution order rationale (TryLoadClass first, Asset Registry fall-back
// for /Game/... parents).
//
// Uses GetAssetsByPackageName (stable across UE 5.6+) rather than the
// version-flavored GetAssetPackageData / TryGetAssetPackageData, so the
// resolver compiles without per-engine-version branches — same convention
// as FAssetRegistrySoftPathResolver (P3.2).
#include "Rules/MissingBlueprintParent/AssetRegistryBlueprintParentResolver.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/Class.h" // TSubclassOf
#include "UObject/SoftObjectPath.h"

namespace
{

// A Blueprint parent path "looks like" a Blueprint when it lives under a
// content mount (/Game/, /PluginName/, ...) rather than the native /Script/
// mount. Used to decide whether a TryLoadClass miss should be classified as
// MissingBlueprintPackage (content-side) or MissingNative.
bool IsBlueprintStylePath(const FString& Path)
{
	return Path.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
		|| Path.StartsWith(TEXT("/Game"), ESearchCase::CaseSensitive);
}

} // namespace

EBlueprintParentResolution FAssetRegistryBlueprintParentResolver::ResolveParent(
	const FString& ParentPath,
	FString& OutReason) const
{
	OutReason.Reset();

	if (ParentPath.IsEmpty())
	{
		OutReason = TEXT("parent class path is empty");
		return EBlueprintParentResolution::MissingNative;
	}

	// TryLoadClass covers both native (/Script/Engine.Actor) and generated
	// Blueprint classes (/Game/BP/BP_Foo.BP_Foo_C). It is cached internally by
	// the soft class path system, so repeat resolves for the same path do not
	// re-load.
	const FSoftClassPath SoftClassPath(ParentPath);
	const TSubclassOf<UObject> Loaded = SoftClassPath.TryLoadClass<UObject>();
	if (Loaded != nullptr)
	{
		return EBlueprintParentResolution::Resolved;
	}

	// TryLoadClass missed. If the parent looks like a Blueprint, ask the Asset
	// Registry whether the parent *package* exists at all — a missing package
	// means the .uasset was deleted (MissingBlueprintPackage); a present
	// package with an unresolvable class points at a deeper compile problem
	// the compile_error rule (P3.4) will own (classified as MissingNative
	// here for v1, since this rule is parent-resolution-only).
	if (IsBlueprintStylePath(ParentPath))
	{
		const FSoftObjectPath Normalized(ParentPath);
		const FName PackageName = Normalized.GetPackageName();

		const FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& Registry = Module.Get();

		TArray<FAssetData> OutAssets;
		if (!Registry.GetAssetsByPackageName(PackageName, OutAssets, /*bIncludeOnlyOnDiskAssets=*/false) || OutAssets.Num() == 0)
		{
			OutReason = FString::Printf(TEXT("Blueprint parent package '%s' is not in the Asset Registry"), *ParentPath);
			return EBlueprintParentResolution::MissingBlueprintPackage;
		}
	}

	OutReason = FString::Printf(TEXT("parent class '%s' did not load as a UClass"), *ParentPath);
	return EBlueprintParentResolution::MissingNative;
}
