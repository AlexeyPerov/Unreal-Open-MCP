// BrokenSoftReferencesScanner - the per-asset soft-reference walk.
//
// Greenfield. Unity's MissingReferences scanner parses serialized YAML to
// extract GUID+fileID pairs; Unreal's identity model is package-path based,
// so the natural seam is: load the package under paths_hint, walk every
// UObject inside, recurse each FProperty looking for FSoftObjectPath values
// (TSoftObjectPtr<> / FSoftObjectPath), and ask the ISoftPathResolver whether
// the target resolves. Unresolved -> one finding.
//
// Why property reflection (not raw .uasset parse):
//   - Binary .uasset packages are not human-readable YAML; parsing them
//     offline is expensive and brittle.
//   - LoadPackage + GetObjectsWithOuter + FProperty walk is the same route
//     the editor uses for reference viewing (-reference viewer, audit), so
//     edge cases (redirectors, subobjects, inherited soft ptrs) line up with
//     what an operator sees in-editor.
//
// ScanPackage is exposed (private header) so Automation specs can drive the
// walk against an in-memory transient UPackage without authoring .uasset
// fixtures on disk.
#pragma once

#include "CoreMinimal.h"

#include "Core/VerifyIssue.h"
#include "Rules/BrokenSoftReferences/ISoftPathResolver.h"

class UPackage;

namespace UnrealOpenMcpVerify::BrokenSoftReferences
{

/**
 * Walk one already-loaded package, collect FSoftObjectPath property values
 * whose target the resolver reports as unresolved, and append one
 * FVerifyIssue per finding into OutIssues.
 *
 * Exposed so tests can drive the walk against a synthetic transient package
 * without going through LoadPackage.
 *
 * @param Package       the loaded package to walk.
 * @param OwnerAssetPath the asset-path form ("/Game/Foo/Bar.Bar") to stamp
 *                      on every emitted finding's AssetPath.
 * @param bFullScan     when false, the scanner skips struct recursion.
 * @param Resolver      resolution seam.
 * @param OutIssues     sink for broken_soft_reference findings.
 */
void ScanPackage(
	UPackage* Package,
	const FString& OwnerAssetPath,
	bool bFullScan,
	const ISoftPathResolver& Resolver,
	TArray<FVerifyIssue>& OutIssues);

/**
 * Walk every package under Scope.Paths via ScanPackage. Loads each package
 * (skipping ones that fail to load -- a missing referencing package is a
 * different rule's concern) and records what it loaded.
 *
 * @param Scope          paths_hint set; each entry is a package path.
 * @param bFullScan      forwarded to ScanPackage.
 * @param Resolver       resolution seam (production = AssetRegistry; tests
 *                       inject a fake).
 * @param OutIssues      sink for broken_soft_reference findings.
 * @param OutLoadedPkgs  packages the scanner loaded (caller bookkeeping).
 */
void ScanScope(
	const FVerifyScope& Scope,
	bool bFullScan,
	const ISoftPathResolver& Resolver,
	TArray<FVerifyIssue>& OutIssues,
	TArray<FName>& OutLoadedPkgs);

} // namespace UnrealOpenMcpVerify::BrokenSoftReferences
