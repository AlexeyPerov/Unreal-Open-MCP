// BrokenSoftReferencesIssueMapper - translate scanner findings into the
// FVerifyIssue surface, mirroring Unity's MissingReferences/IssueMapper role.
//
// Greenfield. Each finding becomes one FVerifyIssue with:
//   RuleId      = broken_soft_references
//   Severity    = Error
//   AssetPath   = the referencing package's long name ("/Game/Foo/Bar.Bar")
//   IssueCode   = broken_soft_reference:<SuffixKey>  (suffix lets a future
//                 fix provider pick which soft pointer to rewrite when an
//                 asset has several)
//   Description = agent-facing copy with the unresolved target
//   Evidence    = assetPath, softPath, propertyName, targetPackage
#pragma once

#include "CoreMinimal.h"

#include "Core/VerifyIssue.h"

namespace UnrealOpenMcpVerify::BrokenSoftReferences
{

// One unresolved soft pointer the scanner collected.
struct FBrokenSoftReferenceFinding
{
	// The referencing asset's long name (e.g. "/Game/Foo/Owner.Owner").
	FString ReferencingAsset;

	// The unresolved soft path as the property held it
	// (e.g. "/Game/Missing/Target.Target").
	FString SoftPath;

	// Property path inside the referencing object (e.g. "Weapon" or
	// "Inventory.Items[0]"). May be empty when the property could not be
	// pinned (rare -- always set by the v1 scanner walk).
	FString PropertyPath;
};

/**
 * Append one FVerifyIssue per finding into OutIssues.
 */
void MapFindingsToIssues(const TArray<FBrokenSoftReferenceFinding>& Findings, TArray<FVerifyIssue>& OutIssues);

} // namespace UnrealOpenMcpVerify::BrokenSoftReferences
