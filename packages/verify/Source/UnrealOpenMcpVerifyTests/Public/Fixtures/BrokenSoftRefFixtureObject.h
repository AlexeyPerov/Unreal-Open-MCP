// Test-only UObject fixture for the broken_soft_references Automation spec.
//
// Lives under the Tests module's Public/ tree so UnrealHeaderTool generates
// reflection data for the soft-pointer properties (a UCLASS declared in a
// .cpp is not seen by UHT). The spec spawns an instance under a transient
// UPackage and drives BrokenSoftReferences::ScanPackage against it.
#pragma once

#include "CoreMinimal.h"

#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/SoftObjectPtr.h"

#include "BrokenSoftRefFixtureObject.generated.h"

/**
 * Carries every soft-pointer shape the broken_soft_references scanner must
 * recognize. Spec-only -- not shipped.
 */
UCLASS()
class UBrokenSoftRefFixtureObject : public UObject
{
	GENERATED_BODY()

public:
	// A pointer the fake resolver reports as resolved.
	UPROPERTY() TSoftObjectPtr<UObject> ValidPtr;
	// A pointer the fake resolver reports as unresolved.
	UPROPERTY() TSoftObjectPtr<UObject> BrokenPtr;
	// An unset pointer -- never a finding.
	UPROPERTY() TSoftObjectPtr<UObject> EmptyPtr;
};
