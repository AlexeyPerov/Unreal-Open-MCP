// MissingBlueprintParentScanner implementation. See header for the load +
// parent-resolution rationale and the ScanBlueprint / ScanScope split.
//
// Detection strategy:
//   - When Blueprint->ParentClass is non-null, capture its path string and
//     ask the IBlueprintParentResolver whether that path resolves. The
//     resolver seam lets tests simulate a missing parent without actually
//     deleting a class (mirrors ISoftPathResolver injection in P3.2).
//   - When Blueprint->ParentClass is null after a successful LoadPackage,
//     the parent import could not be resolved (the UClass* was never
//     materialized, so its path string is lost). Emit a finding with
//     expectedParent = "(unknown)". Asset Registry tags could recover the
//     path in some cases, but tag names vary by engine version; the v1 rule
//     reports the breakage without recovering the original path. A future
//     task may add tag-based recovery.
//
// Per packages/verify/AGENTS.md §Verify rules: Scan() appends only to the
// sink (the runner swallows exceptions). The scanner is side-effect-free
// beyond loading packages the gate flow already paid for.
#include "Rules/MissingBlueprintParent/MissingBlueprintParentScanner.h"

#include "Rules/MissingBlueprintParent/MissingBlueprintParentIssueMapper.h"

#include "Engine/Blueprint.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

namespace UnrealOpenMcpVerify::MissingBlueprintParent
{

namespace
{

// Convert a scope Paths entry into a long package name. Mirrors the
// BrokenSoftReferences scanner's ToLongPackageName helper so the same input
// forms work for both rules:
//   - "/Game/Foo/Bar"          (already a long package name)
//   - "/Game/Foo/Bar.Bar"      (asset path form -- strip the ".Bar" tail)
//   - "/Game/Foo/Bar.uasset"   (file form -- strip the extension)
// Returns empty when the input is empty.
FString ToLongPackageName(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return FString();
	}

	FString Result = Path;

	if (Result.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase) ||
		Result.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
	{
		const int32 Dot = Result.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (Dot != INDEX_NONE)
		{
			Result = Result.Left(Dot);
		}
	}
	else
	{
		// Asset-path form ("/Game/Foo/Bar.Bar") -- strip a single trailing
		// ".<asset-name>" tail. Only strip when the dot is after the last
		// slash so "/Game/Foo.Bar/Baz" still resolves to a package name
		// ("/Game/Foo.Bar/Baz") rather than ("/Game/Foo").
		const int32 LastSlash = Result.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		const int32 Dot = Result.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (Dot != INDEX_NONE && (LastSlash == INDEX_NONE || Dot > LastSlash))
		{
			Result = Result.Left(Dot);
		}
	}

	return Result;
}

// Convert a long package name into the asset-path form used in
// FVerifyIssue.AssetPath ("/Game/Foo/Bar.Bar"). Falls back to the package
// name when the asset name cannot be derived. Mirrors the BrokenSoftReferences
// helper so both rules report asset paths in the same shape.
FString ToAssetPathForm(const FString& LongPackageName)
{
	if (LongPackageName.IsEmpty())
	{
		return LongPackageName;
	}
	const int32 LastSlash = LongPackageName.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (LastSlash == INDEX_NONE)
	{
		return LongPackageName;
	}
	const FString AssetName = LongPackageName.RightChop(LastSlash + 1);
	return FString::Printf(TEXT("%s.%s"), *LongPackageName, *AssetName);
}

} // namespace

void ScanBlueprint(
	UBlueprint* Blueprint,
	const FString& AssetPath,
	const IBlueprintParentResolver& Resolver,
	TArray<FVerifyIssue>& OutIssues)
{
	if (Blueprint == nullptr)
	{
		return;
	}

	UClass* ParentClass = Blueprint->ParentClass;
	if (ParentClass != nullptr)
	{
		// Have a parent class pointer. Capture its path string and ask the
		// resolver whether that path resolves. The resolver seam lets tests
		// simulate a missing parent without actually deleting a class
		// (mirrors ISoftPathResolver injection in P3.2 broken_soft_references).
		const FString ParentPath = ParentClass->GetPathName();
		FString Reason;
		const EBlueprintParentResolution Outcome = Resolver.ResolveParent(ParentPath, Reason);
		if (Outcome == EBlueprintParentResolution::Resolved)
		{
			return; // healthy parent — no finding
		}

		FMissingBlueprintParentFinding Finding;
		Finding.AssetPath = AssetPath;
		Finding.ExpectedParent = ParentPath;
		Finding.Reason = Reason.IsEmpty() ? TEXT("parent class did not resolve") : Reason;
		Finding.Outcome = Outcome;
		MapFindingToIssue(Finding, OutIssues);
		return;
	}

	// ParentClass is null after a successful LoadPackage — the parent import
	// could not be resolved. The UClass* was never materialized, so its path
	// string is lost; emit with an "(unknown)" expected parent and a
	// descriptive reason. Asset Registry tags could recover the path in some
	// cases, but tag names vary by engine version; v1 reports the breakage
	// without recovering the original path.
	FMissingBlueprintParentFinding Finding;
	Finding.AssetPath = AssetPath;
	Finding.ExpectedParent = TEXT("(unknown)");
	Finding.Reason = TEXT("parent class pointer is null after load (parent import could not be resolved)");
	Finding.Outcome = EBlueprintParentResolution::MissingNative;
	MapFindingToIssue(Finding, OutIssues);
}

void ScanScope(
	const FVerifyScope& Scope,
	const IBlueprintParentResolver& Resolver,
	TArray<FVerifyIssue>& OutIssues,
	TArray<FName>& OutLoadedPkgs)
{
	for (const FString& Path : Scope.Paths)
	{
		const FString LongPackageName = ToLongPackageName(Path);
		if (LongPackageName.IsEmpty())
		{
			continue;
		}

		const bool bWasAlreadyLoaded = FindPackage(nullptr, *LongPackageName) != nullptr;
		UPackage* Package = LoadPackage(nullptr, *LongPackageName, LOAD_None);
		if (Package == nullptr)
		{
			// The Blueprint package itself does not load — that is a
			// different class of breakage owned by other rules (P3.4
			// compile_error). Skip silently here so this rule stays focused
			// on broken *parents*.
			continue;
		}
		if (!bWasAlreadyLoaded)
		{
			OutLoadedPkgs.Add(Package->GetFName());
		}

		// Find the top-level UBlueprint asset inside this package. A Blueprint
		// package has exactly one top-level UBlueprint (named after the
		// package); GetObjectsWithOuter with bIncludeNestedObjects=false would
		// miss the redirector-resolved asset in rare cases, so use
		// FindObjectFast via FindObject which follows the standard asset
		// lookup. FindObject<UBlueprint> returns null for non-Blueprint
		// packages — non-Blueprint packages under paths_hint are silently
		// skipped (this rule is parent-resolution-only, not a content-path
		// audit; that is content_path_hygiene territory in P3.7).
		const FString AssetName = LongPackageName.RightChop(LongPackageName.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd) + 1);
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *LongPackageName, *AssetName);
		UBlueprint* Blueprint = FindObject<UBlueprint>(Package, *ObjectPath);
		if (Blueprint == nullptr)
		{
			// Fall back to scanning top-level objects — some Blueprint
			// packages (variants, data-only BPs) name the asset differently
			// than the package. Walk the package's direct children and inspect
			// the first UBlueprint we find.
			TArray<UObject*> TopLevelObjects;
			GetObjectsWithOuter(Package, TopLevelObjects, /*bIncludeNestedObjects=*/false);
			for (UObject* Obj : TopLevelObjects)
			{
				if (UBlueprint* AsBP = Cast<UBlueprint>(Obj))
				{
					Blueprint = AsBP;
					break;
				}
			}
		}

		if (Blueprint == nullptr)
		{
			continue; // not a Blueprint package — not this rule's concern
		}

		ScanBlueprint(Blueprint, ToAssetPathForm(LongPackageName), Resolver, OutIssues);
	}
}

} // namespace UnrealOpenMcpVerify::MissingBlueprintParent
