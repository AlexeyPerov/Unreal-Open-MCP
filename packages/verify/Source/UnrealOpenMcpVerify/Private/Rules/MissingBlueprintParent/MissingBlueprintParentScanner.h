// MissingBlueprintParentScanner - the per-asset parent-resolution walk.
//
// Greenfield. Unity's MissingReferences scanner parses serialized YAML to
// extract MonoBehaviour script GUIDs and checks each one resolves; Unreal's
// inheritance model has no GUID-keyed script slots, so the natural seam is:
// load the package under paths_hint, find the UBlueprint asset inside (the
// top-level asset for a Blueprint package), read Blueprint->ParentClass, and
// ask the IBlueprintParentResolver whether that parent path resolves.
//
// Why UBlueprint (not raw .uasset parse):
//   - Binary .uasset packages are not human-readable YAML; parsing them
//     offline is expensive and brittle, and ParentClass is a UClass* pointer
//     in the serialized UBlueprint — there is no text key to grep.
//   - LoadPackage + FindObject<UBlueprint> + ParentClass is the same route
//     the editor uses for Blueprint parent editing, so edge cases
//     (redirectors, skeleton vs. generated class) line up with what an
//     operator sees in-editor.
//
// ScanPackage is exposed (private header) so Automation specs can drive the
// walk against an in-memory transient UBlueprint without authoring .uasset
// fixtures on disk.
#pragma once

#include "CoreMinimal.h"

#include "Core/VerifyIssue.h"
#include "Rules/MissingBlueprintParent/IBlueprintParentResolver.h"

class UBlueprint;

namespace UnrealOpenMcpVerify::MissingBlueprintParent
{

/**
 * Walk one already-loaded UBlueprint, ask the resolver whether its declared
 * ParentClass resolves, and append one FVerifyIssue into OutIssues when it
 * does not.
 *
 * Exposed so tests can drive the walk against a synthetic transient
 * UBlueprint without going through LoadPackage.
 *
 * @param Blueprint     the Blueprint to inspect (must be non-null).
 * @param AssetPath     the asset-path form ("/Game/Foo/BP_X.BP_X") to stamp
 *                      on every emitted finding's AssetPath.
 * @param Resolver      resolution seam.
 * @param OutIssues     sink for missing_blueprint_parent findings.
 */
void ScanBlueprint(
	UBlueprint* Blueprint,
	const FString& AssetPath,
	const IBlueprintParentResolver& Resolver,
	TArray<FVerifyIssue>& OutIssues);

/**
 * Walk every package under Scope.Paths via ScanBlueprint. Loads each package
 * (skipping ones that fail to load — a missing referencing package is a
 * different rule's concern) and records what it loaded.
 *
 * Only packages that actually contain a UBlueprint asset are inspected; a
 * non-Blueprint package under paths_hint is silently skipped (this rule is
 * parent-resolution-only, not a content-path audit).
 *
 * @param Scope          paths_hint set; each entry is a package path.
 * @param Resolver       resolution seam (production = AssetRegistry; tests
 *                       inject a fake).
 * @param OutIssues      sink for missing_blueprint_parent findings.
 * @param OutLoadedPkgs  packages the scanner loaded (caller bookkeeping).
 */
void ScanScope(
	const FVerifyScope& Scope,
	const IBlueprintParentResolver& Resolver,
	TArray<FVerifyIssue>& OutIssues,
	TArray<FName>& OutLoadedPkgs);

} // namespace UnrealOpenMcpVerify::MissingBlueprintParent
