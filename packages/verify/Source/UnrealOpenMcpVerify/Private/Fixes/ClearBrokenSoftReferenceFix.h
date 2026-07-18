// FClearBrokenSoftReferenceFix — Safe fix provider for the
// broken_soft_references rule.
//
// Greenfield (Unreal has no Unity `remove_missing_script` analogue; the
// closest safe remediation for an unresolved soft object pointer is to clear
// it). The provider nulls the exact FSoftObjectProperty the issue pinned, so
// the asset no longer references a target that cannot load.
//
// Why this is Safe (per packages/verify/AGENTS.md §Fixes — only Safe fixes
// are auto-suggested by the gate):
//   - The soft pointer already resolves to nothing (the rule confirmed it).
//   - We touch exactly one property — the one named in the issue code suffix
//     "<targetPackage>:<propertyPath>". Without a precise property path the
//     provider refuses (returns a structured fix_failed result), so an
//     ambiguous issue is never silently mutated.
//   - The mutation is a single property clear on a loaded package, followed
//     by a deterministic SavePackage. The bridge's ApplyFixGateRunner wraps
//     the apply in a file-level rollback snapshot, so a fix that breaks
//     something else is automatically restored.
//
// v1 scope: top-level FSoftObjectProperty only. A soft pointer nested inside
// a struct (the scanner's depth-1 walk) is reported but cannot be cleared by
// this provider yet — Describe() returns Safe=false in that case so the gate
// will not auto-suggest it. Clearing struct-nested pointers lands with a
// later phase once the property-path grammar supports indexing.
//
// Issue code shape (see BrokenSoftReferencesIssueMapper):
//   broken_soft_reference:<targetPackage>[:<propertyPath>]
//
// The provider reads propertyPath out of the suffix; when the suffix lacks a
// property path, the provider refuses (ambiguous target on the asset).
#pragma once

#include "CoreMinimal.h"

#include "Fixes/FixContracts.h"

/**
 * Safe fix: clear an unresolved soft object pointer so the referencing asset
 * no longer points at a target that cannot load.
 */
class UNREALOPENMCPVERIFY_API FClearBrokenSoftReferenceFix : public IFixProvider
{
public:
	virtual FString GetFixId() const override;
	virtual bool CanFix(const FString& IssueId) const override;
	virtual FFixDescription Describe(const FString& IssueId) const override;
	virtual FFixResult Apply(const FString& IssueId) override;

private:
	/**
	 * Extract the property path the fix should clear from the issue code
	 * suffix. Returns false when the suffix is missing or ambiguous (the
	 * provider then refuses to apply — an ambiguous mutation is never Safe).
	 *
	 * @param IssueId     canonical issue key.
	 * @param OutAssetPath      the referencing asset ("/Game/Foo/Bar.Bar").
	 * @param OutPropertyPath   the property the soft pointer lives on.
	 */
	static bool TryExtractTarget(const FString& IssueId, FString& OutAssetPath, FString& OutPropertyPath);
};
