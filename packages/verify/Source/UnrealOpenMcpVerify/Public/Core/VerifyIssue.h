// FVerifyIssue — one finding emitted by an IVerifyRule.
//
// Ported from Unity Open MCP packages/verify/Editor/Core/VerifyIssue.cs at
// copy fidelity. Fields:
//   RuleId      — the rule's stable Id (e.g. future "broken_soft_references").
//   Severity    — Error or Warning (per-issue, not per-rule).
//   AssetPath   — the offending asset / source path the rule pinned.
//   IssueCode   — stable code linking this issue to its fix(es) and to the
//                 explainability table. v1 sketch (packages/verify/AGENTS.md):
//                 broken_soft_reference, missing_blueprint_parent,
//                 compile_error, content_path_hygiene.
//   Description — human-readable, agent-facing copy.
//   Evidence    — optional per-instance evidence (the specific broken ref /
//                 field / value that fired). Additive and optional — empty
//                 when a rule does not supply it.
//
// The static root-cause + remediation text does NOT live here: it is keyed
// by RuleId|IssueCode in a future explainability table (identical across
// every instance of the same code), so it is not repeated per issue.
#pragma once

#include "CoreMinimal.h"

#include "Core/VerifySeverity.h"

#include "VerifyIssue.generated.h"

USTRUCT(BlueprintType)
struct UNREALOPENMCPVERIFY_API FVerifyIssue
{
	GENERATED_BODY()

	// Stable rule Id. Must match IVerifyRule::GetId() for the rule that
	// emitted this issue.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	FString RuleId;

	// Per-issue severity.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	EVerifySeverity Severity = EVerifySeverity::Warning;

	// Asset / source path the rule pinned. May be empty for project-wide
	// findings (the IssueKey treats an empty asset path as invalid — such
	// issues are reported but cannot be keyed into the gate delta).
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	FString AssetPath;

	// Stable issue code linking this finding to its fix(es). May carry a
	// suffix (e.g. "missing_guid:<guid>") so a fix provider can identify
	// which specific broken reference to rewrite.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	FString IssueCode;

	// Human-readable, agent-facing copy.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	FString Description;

	// Optional per-instance evidence (key → value). Empty when the rule does
	// not supply it. The static root-cause / remediation text is keyed by
	// RuleId|IssueCode elsewhere (identical across instances).
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	TMap<FString, FString> Evidence;

	FVerifyIssue() = default;

	FVerifyIssue(
		FString InRuleId,
		const EVerifySeverity InSeverity,
		FString InAssetPath,
		FString InIssueCode,
		FString InDescription)
		: RuleId(MoveTemp(InRuleId))
		, Severity(InSeverity)
		, AssetPath(MoveTemp(InAssetPath))
		, IssueCode(MoveTemp(InIssueCode))
		, Description(MoveTemp(InDescription))
	{
	}

	FVerifyIssue(
		FString InRuleId,
		const EVerifySeverity InSeverity,
		FString InAssetPath,
		FString InIssueCode,
		FString InDescription,
		TMap<FString, FString> InEvidence)
		: RuleId(MoveTemp(InRuleId))
		, Severity(InSeverity)
		, AssetPath(MoveTemp(InAssetPath))
		, IssueCode(MoveTemp(InIssueCode))
		, Description(MoveTemp(InDescription))
		, Evidence(MoveTemp(InEvidence))
	{
	}
};
