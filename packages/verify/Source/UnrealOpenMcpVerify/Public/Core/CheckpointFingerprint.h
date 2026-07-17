// FCheckpointFingerprint — the pre-mutation snapshot the gate delta compares
// the post-mutation run against.
//
// Ported from Unity Open MCP packages/verify/Editor/Core/CheckpointFingerprint.cs
// at copy fidelity. The fingerprint is per-rule: error / warning counts plus
// the SET of issue keys. The delta (P3.6) reports New / Resolved / Changed
// per rule by diffing the key set.
//
// The checkpoint Id is a short opaque token (cp_xxxxxx) so the gate can
// reference a snapshot in MCP responses without exposing the full fingerprint.
#pragma once

#include "CoreMinimal.h"

#include "CheckpointFingerprint.generated.h"

// Per-rule summary inside a checkpoint.
USTRUCT(BlueprintType)
struct UNREALOPENMCPVERIFY_API FRuleFingerprint
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	int32 Errors = 0;

	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	int32 Warnings = 0;

	// Canonical issue keys (FIssueKey::Build) for every issue this rule
	// emitted at checkpoint time. The delta diffs this set.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	TSet<FString> IssueKeys;

	FRuleFingerprint() = default;

	FRuleFingerprint(const int32 InErrors, const int32 InWarnings, TSet<FString> InIssueKeys)
		: Errors(InErrors)
		, Warnings(InWarnings)
		, IssueKeys(MoveTemp(InIssueKeys))
	{
	}
};

USTRUCT(BlueprintType)
struct UNREALOPENMCPVERIFY_API FCheckpointFingerprint
{
	GENERATED_BODY()

	// Short opaque token (cp_xxxxxx). Used by the gate to reference the
	// snapshot without exposing the full fingerprint.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	FString CheckpointId;

	// Per-rule fingerprints, keyed by rule Id.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	TMap<FString, FRuleFingerprint> Fingerprints;

	FCheckpointFingerprint() = default;

	FCheckpointFingerprint(FString InCheckpointId, TMap<FString, FRuleFingerprint> InFingerprints)
		: CheckpointId(MoveTemp(InCheckpointId))
		, Fingerprints(MoveTemp(InFingerprints))
	{
	}
};
