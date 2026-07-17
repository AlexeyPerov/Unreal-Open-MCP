// FVerifyScope — bounds which packages / asset paths a verify run considers.
//
// Ported from Unity Open MCP packages/verify/Editor/Core/VerifyScope.cs.
// `Paths` is the paths_hint array (content paths like /Game/Foo/Bar.uasset,
// source paths under Source/, or package names); `bIncludeDependents` opts
// the runner into walking the forward-dependency closure (used by the
// dependencies rule, not by the P3.1 scaffold).
#pragma once

#include "CoreMinimal.h"

#include "VerifyScope.generated.h"

USTRUCT(BlueprintType)
struct UNREALOPENMCPVERIFY_API FVerifyScope
{
	GENERATED_BODY()

	// Asset / source / package paths bounding this run. Empty array means
	// "whole project" (Full scans); a non-empty array is the paths_hint set
	// the gate feeds in for a scoped Validate pass.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	TArray<FString> Paths;

	// When true, the runner may expand Paths into their forward-dependency
	// closure (dependencies rule). Off by default so a scoped Validate stays
	// bounded to the touched paths.
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	bool bIncludeDependents = false;

	FVerifyScope() = default;

	explicit FVerifyScope(TArray<FString> InPaths, const bool bInIncludeDependents = false)
		: Paths(MoveTemp(InPaths))
		, bIncludeDependents(bInIncludeDependents)
	{
	}
};
