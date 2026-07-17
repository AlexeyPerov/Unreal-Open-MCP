// FAssetRegistrySoftPathResolver - production ISoftPathResolver backed by the
// Asset Registry. Greenfield.
//
// Resolution strategy (in order, short-circuit on first definitive answer):
//   1. Normalize the input through FSoftObjectPath so the package name is
//      well-formed. Inputs the scanner hands us are already package paths,
//      but going through FSoftObjectPath defends against the occasional full
//      "package.asset" or "package.asset:subobject" form.
//   2. AssetRegistry.GetAssetsByPackageName(PackageName, ...). Non-empty ->
//      resolved. GetAssetsByPackageName is stable across UE 5.6+ and avoids
//      the version-flavored GetAssetPackageData / TryGetAssetPackageData
//      signature split.
//   3. (Fall-back) TryLoad on the asset -- covers assets the registry hasn't
//      discovered yet (e.g. freshly saved but not rescanned). Cached
//      internally by the soft object path system.
//
// Why not just TryLoad: TryLoad forces a package into memory every time,
// which is the editor hitch we are trying to avoid during a scoped Validate.
// The Asset Registry is the O(1) path; TryLoad is the rare fall-back only.
#pragma once

#include "CoreMinimal.h"

#include "Rules/BrokenSoftReferences/ISoftPathResolver.h"

class FAssetRegistrySoftPathResolver final : public ISoftPathResolver
{
public:
	FAssetRegistrySoftPathResolver() = default;

	virtual bool Resolve(const FString& SoftPath) const override;
};
