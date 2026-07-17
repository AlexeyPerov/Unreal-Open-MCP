// ISoftPathResolver — the seam the broken_soft_references scanner uses to ask
// "does this soft object path resolve to a loadable asset?".
//
// The production resolver (FAssetRegistrySoftPathResolver) queries the Asset
// Registry and a lightweight load check; tests inject a fake so the rule can
// be exercised against synthetic resolution maps without authoring .uasset
// fixtures. Mirrors how Unity's MissingReferences Scanner takes a
// GUID-to-path resolver, but adapted to Unreal's package-path identity model
// (no GUIDs in the user-facing sense).
//
// Plain abstract C++ class (not a UObject) — see IVerifyRule.h for the same
// convention.
#pragma once

#include "CoreMinimal.h"

/**
 * Answer whether a soft object path resolves to an asset the Asset Registry
 * knows about AND that can be loaded. Implementations must be side-effect-free
 * beyond any caching they perform (the scanner runs on the game thread inside
 * the gate flow).
 */
class UNREALOPENMCPVERIFY_API ISoftPathResolver
{
public:
	virtual ~ISoftPathResolver() = default;

	/**
	 * @param SoftPath  a top-level asset path like "/Game/Foo/Bar.Bar" or a
	 *                  package path like "/Game/Foo/Bar". Forwarded verbatim
	 *                  from the rule's property walk.
	 * @return true when the target exists in the Asset Registry (or loads).
	 *         Implementations must never throw — the runner swallows
	 *         exceptions, but a resolver that throws will still abort the
	 *         current rule's remaining work in no-exception builds.
	 */
	virtual bool Resolve(const FString& SoftPath) const = 0;
};
