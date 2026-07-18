// IBlueprintParentResolver — the seam the missing_blueprint_parents scanner
// uses to ask "does this Blueprint's declared parent class resolve?".
//
// Mirrors the ISoftPathResolver pattern (P3.2 broken_soft_references): the
// production resolver queries the Asset Registry and the class loading
// system; tests inject a fake so the rule can be exercised against synthetic
// resolution maps without authoring .uasset fixtures on disk.
//
// Resolution is path-based (Unreal identity model; no GUIDs in the
// user-facing sense). The scanner hands the resolver the Blueprint's
// declared parent path (a /Script/... native ref or a /Game/.../BP_X.BP_X_C
// generated-class ref); the resolver answers whether that parent can be
// loaded.
//
// Plain abstract C++ class (not a UObject) — see IVerifyRule.h for the same
// convention.
#pragma once

#include "CoreMinimal.h"

/**
 * Resolution outcome for a Blueprint parent class. The scanner maps each
 * outcome to evidence fields on the emitted issue.
 */
enum class EBlueprintParentResolution : uint8
{
	// The parent class loads successfully (Blueprint is healthy for this rule).
	Resolved = 0,
	// The parent is a missing native class (/Script/... that no module
	// provides — typically a plugin was removed).
	MissingNative = 1,
	// The parent is a missing Blueprint package (/Game/.../Parent.Parent that
	// the Asset Registry does not know — the .uasset was deleted).
	MissingBlueprintPackage = 2,
};

/**
 * Answer whether a Blueprint's declared parent class resolves to a loadable
 * UClass, and if not, why. Implementations must be side-effect-free beyond
 * any caching they perform (the scanner runs on the game thread inside the
 * gate flow).
 */
class UNREALOPENMCPVERIFY_API IBlueprintParentResolver
{
public:
	virtual ~IBlueprintParentResolver() = default;

	/**
	 * @param ParentPath   the declared parent class path
	 *                     (e.g. "/Script/Engine.Actor",
	 *                      "/Game/BP/BP_Foo.BP_Foo_C").
	 * @param OutReason    when the result is not Resolved, optionally filled
	 *                     with a short human-readable reason for evidence
	 *                     ("native class not found", "package missing", …).
	 * @return the resolution outcome.
	 *
	 * Implementations must never throw — the runner swallows exceptions, but
	 * a resolver that throws will still abort the current rule's remaining
	 * work in no-exception builds.
	 */
	virtual EBlueprintParentResolution ResolveParent(
		const FString& ParentPath,
		FString& OutReason) const = 0;
};
