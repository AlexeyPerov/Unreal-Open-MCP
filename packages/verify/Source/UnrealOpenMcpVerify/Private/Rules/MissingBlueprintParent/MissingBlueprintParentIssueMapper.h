// MissingBlueprintParentIssueMapper - translate scanner findings into the
// FVerifyIssue surface, mirroring Unity's MissingReferences/IssueMapper role.
//
// Greenfield. Each finding becomes one FVerifyIssue with:
//   RuleId      = missing_blueprint_parents
//   Severity    = Error
//   AssetPath   = the Blueprint's asset path ("/Game/Foo/BP_X.BP_X")
//   IssueCode   = missing_blueprint_parent:<ExpectedParent>  (suffix lets a
//                 future fix provider pick which parent to repair; today one
//                 per asset, but the contract is suffix-ready)
//   Description = agent-facing copy with the unresolved parent
//   Evidence    = assetPath, expectedParent, reason
#pragma once

#include "CoreMinimal.h"

#include "Core/VerifyIssue.h"
#include "Rules/MissingBlueprintParent/IBlueprintParentResolver.h"

namespace UnrealOpenMcpVerify::MissingBlueprintParent
{

// One unresolved-parent finding the scanner collected.
struct FMissingBlueprintParentFinding
{
	// The Blueprint's asset path (e.g. "/Game/Foo/BP_X.BP_X").
	FString AssetPath;

	// The declared parent class path (e.g. "/Script/Engine.Actor",
	// "/Game/BP/BP_Foo.BP_Foo_C"). "(unknown)" when ParentClass was null and
	// the path could not be recovered.
	FString ExpectedParent;

	// Short human-readable reason ("native class not found",
	// "package missing", …). Always non-empty.
	FString Reason;

	// Classifier from the resolver. Drives the description wording so a
	// native-miss reads differently from a deleted-package miss.
	EBlueprintParentResolution Outcome = EBlueprintParentResolution::MissingNative;
};

/**
 * Append one FVerifyIssue per finding into OutIssues.
 *
 * Single-finding helper (not a batch Map) because the v1 scanner emits at
 * most one finding per Blueprint, but the function shape mirrors the
 * BrokenSoftReferences MapFindingsToIssues for consistency if multiple
 * parents ever need reporting.
 */
void MapFindingToIssue(const FMissingBlueprintParentFinding& Finding, TArray<FVerifyIssue>& OutIssues);

} // namespace UnrealOpenMcpVerify::MissingBlueprintParent
