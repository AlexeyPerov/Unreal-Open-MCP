// BrokenSoftReferencesScanner implementation. See header for the load +
// property-walk rationale and the ScanPackage / ScanScope split.
#include "Rules/BrokenSoftReferences/BrokenSoftReferencesScanner.h"

#include "Rules/BrokenSoftReferences/BrokenSoftReferencesIssueMapper.h"

#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Package.h"
#include "UObject/Property.h"
#include "UObject/PropertySoftObject.h"
#include "UObject/PropertyStruct.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

namespace UnrealOpenMcpVerify::BrokenSoftReferences
{

namespace
{

// Convert a scope Paths entry into a long package name ("/Game/Foo/Bar").
// Accepts:
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

	// Drop a trailing .uasset / .umap extension.
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
// name when the asset name cannot be derived.
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

// Collect FSoftObjectPath values held by an object via property reflection.
// Recurses one level into struct-typed properties so common patterns like
// "ItemHandle.SoftItem" still surface. Depth-bounded to avoid runaway walks
// on deeply nested structs.
void CollectSoftObjectPaths(
	UObject* Object,
	const FString& OwnerAssetPath,
	const FString& PropertyPrefix,
	int32 Depth,
	const ISoftPathResolver& Resolver,
	TArray<FBrokenSoftReferenceFinding>& OutFindings)
{
	if (Object == nullptr)
	{
		return;
	}

	UClass* Class = Object->GetClass();
	if (Class == nullptr)
	{
		return;
	}

	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		FProperty* Prop = *It;
		if (Prop == nullptr)
		{
			continue;
		}

		const FString PropertyName = Prop->GetName();
		const FString PropertyPath = PropertyPrefix.IsEmpty()
			? PropertyName
			: FString::Printf(TEXT("%s.%s"), *PropertyPrefix, *PropertyName);

		// FSoftObjectProperty covers TSoftObjectPtr<T> and FSoftObjectPath.
		// FSoftClassProperty (TSoftClassPtr<T>) is a subclass, so the same
		// branch catches soft class pointers too.
		if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
		{
			const void* ValueAddr = SoftProp->ContainerPtrToValuePtr<void>(Object);
			if (ValueAddr == nullptr)
			{
				continue;
			}
			const FSoftObjectPath SoftPath = SoftProp->GetPropertyValue(ValueAddr);
			const FString PathString = SoftPath.ToString();
			if (PathString.IsEmpty())
			{
				// An unset soft pointer is not a breakage -- skip.
				continue;
			}
			if (!Resolver.Resolve(PathString))
			{
				OutFindings.Add({OwnerAssetPath, PathString, PropertyPath});
			}
			continue;
		}

		// Recurse into struct properties (depth-bounded).
		if (Depth > 0)
		{
			if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				void* StructAddr = StructProp->ContainerPtrToValuePtr<void>(Object);
				if (StructAddr == nullptr)
				{
					continue;
				}
				UScriptStruct* Struct = StructProp->Struct;
				if (Struct == nullptr)
				{
					continue;
				}
				// Walk the struct's fields directly (a UScriptStruct is a
				// UClass-like field owner).
				for (TFieldIterator<FProperty> SIt(Struct); SIt; ++SIt)
				{
					FProperty* InnerProp = *SIt;
					if (InnerProp == nullptr)
					{
						continue;
					}
					if (FSoftObjectProperty* InnerSoft = CastField<FSoftObjectProperty>(InnerProp))
					{
						const void* InnerAddr = InnerSoft->ContainerPtrToValuePtr<void>(StructAddr);
						if (InnerAddr == nullptr)
						{
							continue;
						}
						const FSoftObjectPath InnerPath = InnerSoft->GetPropertyValue(InnerAddr);
						const FString PathString = InnerPath.ToString();
						if (PathString.IsEmpty())
						{
							continue;
						}
						if (!Resolver.Resolve(PathString))
						{
							const FString InnerPathField = FString::Printf(TEXT("%s.%s"), *PropertyPath, *InnerProp->GetName());
							OutFindings.Add({OwnerAssetPath, PathString, InnerPathField});
						}
					}
				}
			}
		}
	}
}

} // namespace

void ScanPackage(
	UPackage* Package,
	const FString& OwnerAssetPath,
	bool bFullScan,
	const ISoftPathResolver& Resolver,
	TArray<FVerifyIssue>& OutIssues)
{
	if (Package == nullptr)
	{
		return;
	}

	TArray<FBrokenSoftReferenceFinding> Findings;

	TArray<UObject*> Objects;
	GetObjectsWithOuter(Package, Objects, /*bIncludeNestedObjects=*/true);

	const int32 StructDepth = bFullScan ? 1 : 0;
	for (UObject* Obj : Objects)
	{
		if (Obj == nullptr)
		{
			continue;
		}
		// Skip the package's CDO and archetypes -- their soft pointers are
		// class defaults and a separate concern. (Worth revisiting later if
		// a project ships broken CDO soft refs; out of scope for v1.)
		if (Obj->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		{
			continue;
		}
		CollectSoftObjectPaths(Obj, OwnerAssetPath, FString(), StructDepth, Resolver, Findings);
	}

	MapFindingsToIssues(Findings, OutIssues);
}

void ScanScope(
	const FVerifyScope& Scope,
	bool bFullScan,
	const ISoftPathResolver& Resolver,
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
			// The referencing package itself does not load -- that is a
			// different class of breakage (e.g. missing native dependency)
			// owned by other rules (P3.4 compile_error). Skip silently here
			// so this rule stays focused on broken *targets*.
			continue;
		}
		if (!bWasAlreadyLoaded)
		{
			OutLoadedPkgs.Add(Package->GetFName());
		}

		ScanPackage(Package, ToAssetPathForm(LongPackageName), bFullScan, Resolver, OutIssues);
	}
}

} // namespace UnrealOpenMcpVerify::BrokenSoftReferences
