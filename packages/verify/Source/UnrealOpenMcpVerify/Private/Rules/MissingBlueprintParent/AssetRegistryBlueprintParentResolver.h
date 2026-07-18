// FAssetRegistryBlueprintParentResolver - production IBlueprintParentResolver
// backed by the Asset Registry + UObject class loading. Greenfield.
//
// Resolution strategy (in order, short-circuit on first definitive answer):
//   1. Empty input -> MissingNative (defensive; the scanner should never
//      hand us an empty string, but a clean resolution keeps the scan quiet
//      in no-exception builds).
//   2. FSoftClassPath::TryLoadClass. Covers both native (/Script/Engine.Actor)
//      and generated Blueprint classes (/Game/BP/BP_Foo.BP_Foo_C). Cached
//      internally by the soft class path system so repeat calls for the same
//      path do not re-load.
//   3. If TryLoadClass failed AND the path looks like a /Game/... Blueprint
//      parent, ask the Asset Registry whether the parent package exists at
//      all. Missing package -> MissingBlueprintPackage; present package but
//      still no resolvable class -> MissingNative (a subobject/compile
//      failure that the compile_error rule P3.4 will own; here we just say
//      "the parent class itself did not resolve as a native class").
//
// Why not just TryLoadClass: TryLoadClass emits a LogUObjectGlobals warning
// on miss (the same noise UnrealMCP's ResolveClass suppresses via
// LOAD_NoWarn | LOAD_Quiet). We accept the warning here because the Asset
// Registry fall-back is the cheaper first path for Blueprint parents, and
// TryLoadClass is only the verification step after a package is known to
// exist.
#pragma once

#include "CoreMinimal.h"

#include "Rules/MissingBlueprintParent/IBlueprintParentResolver.h"

class FAssetRegistryBlueprintParentResolver final : public IBlueprintParentResolver
{
public:
	FAssetRegistryBlueprintParentResolver() = default;

	virtual EBlueprintParentResolution ResolveParent(
		const FString& ParentPath,
		FString& OutReason) const override;
};
