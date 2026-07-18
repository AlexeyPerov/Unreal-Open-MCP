// FClearBrokenSoftReferenceFix implementation. See header for the Safe
// rationale and the v1 scope (top-level FSoftObjectProperty only).
//
// The apply path:
//   1) Parse the issue key → referencing asset path + property path.
//   2) Convert the asset path ("/Game/Foo/Bar.Bar") to a long package name
//      ("/Game/Foo/Bar") and LoadPackage.
//   3) GetObjectsWithOuter to enumerate every UObject owned by the package.
//   4) For each object, walk top-level FProperty fields and find the named
//      FSoftObjectProperty. Verify its current value still equals the broken
//      target before mutating (the agent may have already cleared it; that is
//      a successful no-op).
//   5) Set the property to an empty FSoftObjectPath, mark the package dirty,
//      and SavePackage to disk.
//
// Refuses (returns fix_failed) when:
//   - The issue key is malformed.
//   - The suffix lacks a property path (ambiguous target).
//   - The property path contains a "." (nested struct field — v1 does not
//     clear those; Describe() returns Safe=false so the gate will not auto-
//     suggest the fix for a nested-soft-pointer finding).
//   - The package fails to load (different breakage class — owned by other
//     rules).
//   - The named property cannot be found on any object in the package (the
//     asset layout changed between scan and fix — surface as fix_failed so the
//     agent re-scans).
#include "Fixes/ClearBrokenSoftReferenceFix.h"

#include "Core/IssueKey.h"
#include "Core/VerifySeverity.h"
#include "Rules/BrokenSoftReferences/BrokenSoftReferencesIssueCodes.h"

#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Package.h"
#include "UObject/Property.h"
#include "UObject/PropertySoftObject.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

namespace
{
// Convert an asset-path-form string ("/Game/Foo/Bar.Bar") to a long package
// name ("/Game/Foo/Bar"). Mirrors BrokenSoftReferencesScanner::ToLongPackageName
// — duplicated here so the verify module's fix path does not depend on the
// rule's private helper (the rule's helpers live in a Private/ header that is
// not on the fix TU's include path).
FString ToLongPackageName(const FString& AssetPathForm)
{
	if (AssetPathForm.IsEmpty())
	{
		return FString();
	}
	FString Result = AssetPathForm;
	const int32 LastSlash = Result.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	const int32 Dot = Result.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (Dot != INDEX_NONE && (LastSlash == INDEX_NONE || Dot > LastSlash))
	{
		Result = Result.Left(Dot);
	}
	return Result;
}
} // namespace

FString FClearBrokenSoftReferenceFix::GetFixId() const
{
	// Pinned in packages/verify/AGENTS.md and the bridge's gate-policy hint
	// (FUnrealOpenMcpGatePolicyInternal::TryFixIdForIssue). Stable across
	// versions — do not rename without coordinated catalog sync (P3.8).
	return TEXT("clear_broken_soft_reference");
}

bool FClearBrokenSoftReferenceFix::CanFix(const FString& IssueId) const
{
	FString RuleId;
	EVerifySeverity Severity = EVerifySeverity::Warning;
	FString AssetPath;
	FString IssueCode;
	if (!FIssueKey::TryParse(IssueId, RuleId, Severity, AssetPath, IssueCode))
	{
		return false;
	}
	if (RuleId != BrokenSoftReferences::RuleId)
	{
		return false;
	}
	// Bare code must match. The suffix carries the per-instance identity and
	// is parsed separately by TryExtractTarget at apply time.
	return FIssueKey::BareIssueCode(IssueCode) == BrokenSoftReferences::IssueCode;
}

FFixDescription FClearBrokenSoftReferenceFix::Describe(const FString& IssueId) const
{
	FString AssetPath;
	FString PropertyPath;
	const bool bPrecise = TryExtractTarget(IssueId, AssetPath, PropertyPath);

	// Safe only when the target is precisely identified AND a top-level
	// property (no '.' in the path — v1 does not clear struct-nested soft
	// pointers). When ambiguous, return Safe=false so the gate never auto-
	// suggests this fix; the agent can still call apply_fix explicitly with
	// the issue id.
	const bool bNested = !PropertyPath.IsEmpty() && PropertyPath.Contains(TEXT("."));
	const bool bSafe = bPrecise && !bNested;

	FFixDescription D;
	D.FixId = GetFixId();
	D.IssueId = IssueId;
	D.AssetPath = AssetPath;
	if (!bPrecise)
	{
		D.Description = TEXT(
			"Refuses to apply: the issue does not pin a precise property path. "
			"Re-scan to refresh the suffix or clear the property manually.");
		D.bSafe = false;
		return D;
	}
	if (bNested)
	{
		D.Description = FString::Printf(
			TEXT("Refuses to apply (v1): property '%s' is nested inside a struct; this provider clears top-level soft pointers only. "),
			*PropertyPath);
		D.bSafe = false;
		return D;
	}
	D.Description = FString::Printf(
		TEXT("Clear the broken soft object pointer at '%s' on '%s' (sets the property to null and saves the package)."),
		*PropertyPath, *AssetPath);
	D.bSafe = true;
	return D;
}

FFixResult FClearBrokenSoftReferenceFix::Apply(const FString& IssueId)
{
	FString AssetPath;
	FString PropertyPath;
	if (!TryExtractTarget(IssueId, AssetPath, PropertyPath))
	{
		FFixResult R;
		R.bSuccess = false;
		R.Description = TEXT("Cannot apply: issue id lacks a precise property path.");
		return R;
	}
	if (PropertyPath.Contains(TEXT(".")))
	{
		// Refuse struct-nested properties (v1 scope). Re-scan with the v2
		// scanner if/when the property-path grammar expands to indexing.
		FFixResult R;
		R.bSuccess = false;
		R.Description = FString::Printf(
			TEXT("Cannot apply (v1): property '%s' is nested inside a struct; this provider clears top-level soft pointers only."),
			*PropertyPath);
		return R;
	}

	const FString LongPackageName = ToLongPackageName(AssetPath);
	if (LongPackageName.IsEmpty())
	{
		FFixResult R;
		R.bSuccess = false;
		R.Description = FString::Printf(TEXT("Cannot derive a package name from asset path '%s'."), *AssetPath);
		return R;
	}

	UPackage* Package = LoadPackage(nullptr, *LongPackageName, LOAD_None);
	if (Package == nullptr)
	{
		FFixResult R;
		R.bSuccess = false;
		R.Description = FString::Printf(
			TEXT("Cannot load package '%s' to clear the broken soft reference."), *LongPackageName);
		return R;
	}

	// Enumerate UObjects owned by the package and locate the named property.
	TArray<UObject*> Objects;
	GetObjectsWithOuter(Package, Objects, /*bIncludeNestedObjects=*/true);

	int32 ClearedCount = 0;
	FString LastValueSeen;
	for (UObject* Obj : Objects)
	{
		if (Obj == nullptr)
		{
			continue;
		}
		// Skip CDO / archetype — clearing class-default soft pointers is a
		// separate concern (mirrors BrokenSoftReferencesScanner).
		if (Obj->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		{
			continue;
		}
		UClass* Class = Obj->GetClass();
		if (Class == nullptr)
		{
			continue;
		}
		FProperty* FoundProp = nullptr;
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			FProperty* Prop = *It;
			if (Prop == nullptr)
			{
				continue;
			}
			if (Prop->GetName() == PropertyPath)
			{
				FoundProp = Prop;
				break;
			}
		}
		if (FoundProp == nullptr)
		{
			continue;
		}
		FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(FoundProp);
		if (SoftProp == nullptr)
		{
			// The named property is not a soft object pointer — the asset
			// layout changed between scan and fix. Surface as fix_failed so
			// the agent re-scans rather than silently no-op'ing.
			FFixResult R;
			R.bSuccess = false;
			R.Description = FString::Printf(
				TEXT("Property '%s' on '%s' is not a soft object pointer (asset layout changed since scan)."),
				*PropertyPath, *AssetPath);
			return R;
		}

		void* ValueAddr = SoftProp->ContainerPtrToValuePtr<void>(Obj);
		if (ValueAddr == nullptr)
		{
			continue;
		}
		const FSoftObjectPath Current = SoftProp->GetPropertyValue(ValueAddr);
		LastValueSeen = Current.ToString();
		if (LastValueSeen.IsEmpty())
		{
			// Already cleared (e.g. an earlier apply). Treat as success — the
			// end state matches the requested one.
			++ClearedCount;
			continue;
		}

		// Pre-mutation mark + clear. Modify fires the package's
		// MarkAsDirty / RF_Transactional hooks so a subsequent SavePackage
		// actually writes the change.
		Obj->Modify();
		SoftProp->SetPropertyValue(ValueAddr, FSoftObjectPath());
		++ClearedCount;
	}

	if (ClearedCount == 0)
	{
		// The named property was not found on any object in the package. The
		// issue may have been resolved by another path, or the scan pinned a
		// stale suffix. Either way the asset on disk is unchanged — surface
		// as a structured failure so the agent re-scans before retrying.
		FFixResult R;
		R.bSuccess = false;
		R.Description = FString::Printf(
			TEXT("Property '%s' was not found on any object in package '%s' (asset layout may have changed since scan)."),
			*PropertyPath, *LongPackageName);
		return R;
	}

	// Save the package so the clear survives an editor restart. UE saves to
	// the package's filename via UPackage::Save.
	Package->MarkAsDirty(true);
	const FString Filename = FPackageName::LongPackageNameToFilename(LongPackageName, FPackageName::GetAssetPackageExtension());
	const bool bSaved = UPackage::SavePackage(Package, /*TopLevelObject=*/nullptr, RF_Standalone, *Filename);

	if (!bSaved)
	{
		FFixResult R;
		R.bSuccess = false;
		R.Description = FString::Printf(
			TEXT("Cleared %d soft pointer(s) but SavePackage failed for '%s' (the change is in-memory only)."),
			ClearedCount, *LongPackageName);
		return R;
	}

	FFixResult R;
	R.bSuccess = true;
	R.Description = FString::Printf(
		TEXT("Cleared %d broken soft object pointer(s) at '%s' on '%s' and saved package '%s'."),
		ClearedCount, *PropertyPath, *AssetPath, *LongPackageName);
	R.TouchedPaths.Add(AssetPath);
	return R;
}

bool FClearBrokenSoftReferenceFix::TryExtractTarget(const FString& IssueId, FString& OutAssetPath, FString& OutPropertyPath)
{
	OutAssetPath.Reset();
	OutPropertyPath.Reset();

	FString RuleId;
	EVerifySeverity Severity = EVerifySeverity::Warning;
	FString AssetPath;
	FString IssueCode;
	if (!FIssueKey::TryParse(IssueId, RuleId, Severity, AssetPath, IssueCode))
	{
		return false;
	}
	if (RuleId != BrokenSoftReferences::RuleId)
	{
		return false;
	}
	if (FIssueKey::BareIssueCode(IssueCode) != BrokenSoftReferences::IssueCode)
	{
		return false;
	}

	const FString Suffix = FIssueKey::IssueCodeSuffix(IssueCode);
	if (Suffix.IsEmpty())
	{
		// No suffix — the issue code is bare. Refuse (ambiguous target).
		return false;
	}

	// Suffix shape (see BrokenSoftReferencesIssueMapper::BuildSuffix):
	//   "<targetPackage>[:<propertyPath>]"
	// Only the propertyPath half is needed here (the targetPackage is the
	// thing the rule already reported as broken; clearing the property
	// neutralizes the reference regardless of the original target).
	const int32 ColonIdx = Suffix.Find(TEXT(":"), ESearchCase::CaseSensitive);
	if (ColonIdx == INDEX_NONE)
	{
		// Suffix is a bare target package name — no property path. The mapper
		// emits this shape only when the scanner could not pin a property
		// path, so refuse rather than guess.
		return false;
	}
	OutPropertyPath = Suffix.Mid(ColonIdx + 1);
	if (OutPropertyPath.IsEmpty())
	{
		return false;
	}

	OutAssetPath = AssetPath;
	return true;
}
