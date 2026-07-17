// FVerifyResult — outcome of one FVerifyRunner::RunScoped pass.
//
// Ported from Unity Open MCP packages/verify/Editor/Core/VerifyResult.cs at
// copy fidelity. Carries the issues found, the rule Ids actually run, the
// wall-clock duration, and the unknown/available rule Id rosters (so a
// caller asking for a non-existent rule Id gets a structured hint back).
#pragma once

#include "CoreMinimal.h"

#include "Core/VerifyIssue.h"

#include "VerifyResult.generated.h"

USTRUCT(BlueprintType)
struct UNREALOPENMCPVERIFY_API FVerifyResult
{
	GENERATED_BODY()

	// Issues emitted by the rules that ran.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	TArray<FVerifyIssue> Issues;

	// Rule Ids whose Scan() was actually invoked (subset of registered rules
	// when the caller narrowed via RuleIds).
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	TArray<FString> CategoriesRun;

	// Wall-clock duration of RunScoped, milliseconds.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	int64 DurationMs = 0;

	// Rule Ids the caller asked for that are NOT registered. Empty when the
	// caller asked for all rules (RuleIds empty) or every requested Id was
	// known.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	TArray<FString> UnknownRuleIds;

	// Every registered rule Id at the time of the run. Lets a caller report
	// "did you mean …?" alongside UnknownRuleIds.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	TArray<FString> AvailableRuleIds;

	FVerifyResult() = default;

	FVerifyResult(
		TArray<FVerifyIssue> InIssues,
		TArray<FString> InCategoriesRun,
		const int64 InDurationMs)
		: Issues(MoveTemp(InIssues))
		, CategoriesRun(MoveTemp(InCategoriesRun))
		, DurationMs(InDurationMs)
	{
	}

	FVerifyResult(
		TArray<FVerifyIssue> InIssues,
		TArray<FString> InCategoriesRun,
		const int64 InDurationMs,
		TArray<FString> InUnknownRuleIds,
		TArray<FString> InAvailableRuleIds)
		: Issues(MoveTemp(InIssues))
		, CategoriesRun(MoveTemp(InCategoriesRun))
		, DurationMs(InDurationMs)
		, UnknownRuleIds(MoveTemp(InUnknownRuleIds))
		, AvailableRuleIds(MoveTemp(InAvailableRuleIds))
	{
	}

	bool HasUnknownRules() const { return UnknownRuleIds.Num() > 0; }
};
