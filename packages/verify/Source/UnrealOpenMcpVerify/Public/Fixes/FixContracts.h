// Fix-side contracts: FFixDescription, FFixResult, FFixCandidate, IFixProvider.
//
// Ported from Unity Open MCP packages/verify/Editor/Fixes/FixProviderRegistry.cs
// (the result / provider types) at copy fidelity. Every fix implements
// IFixProvider and registers via FFixProviderRegistry. Each fix declares a
// stable FixId and implements CanFix(issueId). Fixes marked Safe are the
// only ones the gate will auto-suggest; unsafe fixes (e.g. relink) require a
// deliberate target argument from the caller.
//
// Plain C++ types (not UObjects) — the gate (P3.7) holds providers by
// TUniquePtr<IFixProvider>.
#pragma once

#include "CoreMinimal.h"

#include "FixContracts.generated.h"

/** Static description of a fix for a specific issue. */
USTRUCT(BlueprintType)
struct UNREALOPENMCPVERIFY_API FFixDescription
{
	GENERATED_BODY()

	// Stable fix Id (e.g. "remove_missing_script", "relink_broken_guid").
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	FString FixId;

	// Issue this fix would resolve (canonical issue key).
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	FString IssueId;

	// Asset / source path the fix would touch.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	FString AssetPath;

	// Human-readable, agent-facing copy.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	FString Description;

	// Safe fixes are the only ones the gate will auto-suggest. Unsafe fixes
	// (e.g. relink_broken_guid) require a deliberate target argument.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	bool bSafe = false;
};

/** Outcome of running a fix. */
USTRUCT(BlueprintType)
struct UNREALOPENMCPVERIFY_API FFixResult
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	bool bSuccess = false;

	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	FString Description;

	// Paths the fix touched — used by the gate delta to scope the post-fix
	// validation pass.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	TArray<FString> TouchedPaths;
};

/**
 * A fix candidate the gate advertises alongside an issue so an agent sees
 * every option (safe vs unsafe) in one pass, not just the first match
 * TryGetFixInfo returns. bSafe mirrors the provider's Describe().
 */
USTRUCT(BlueprintType)
struct UNREALOPENMCPVERIFY_API FFixCandidate
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	FString FixId;

	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	bool bSafe = false;
};

/**
 * Contract every fix provider implements.
 */
class UNREALOPENMCPVERIFY_API IFixProvider
{
public:
	virtual ~IFixProvider() = default;

	/** Stable fix Id (e.g. "remove_missing_script"). */
	virtual FString GetFixId() const = 0;

	/** True when this provider can resolve the supplied issue key. */
	virtual bool CanFix(const FString& IssueId) const = 0;

	/** Describe the fix for a specific issue (used to populate the gate ad). */
	virtual FFixDescription Describe(const FString& IssueId) const = 0;

	/** Apply the fix to the supplied issue. */
	virtual FFixResult Apply(const FString& IssueId) = 0;
};
