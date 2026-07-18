// Test-only fixture for the missing_blueprint_parents Automation spec.
//
// The spec cannot easily construct a transient UBlueprint with a controlled
// ParentClass pointer through NewObject (UBlueprint's construction has
// non-trivial FBlueprintEditor machinery). Instead the spec exercises the
// rule's resolver seam directly via ScanBlueprint on a synthesized UBlueprint
// whose ParentClass is wired to a transient UClass or null.
//
// This header exists only so UHT generates the simple UObject subclass used
// as a stand-in UClass target for ParentClass when a "non-null but
// unresolved" case is needed. The class itself is not part of the rule's
// runtime; it is the resolution target the fake resolver pretends to fail
// to load.
#pragma once

#include "CoreMinimal.h"

#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "MissingBlueprintParentFixtureObject.generated.h"

/**
 * Spec-only stand-in UObject used as a transient parent class target. The
 * fake resolver reports the path of an instance of this class as either
 * resolved or unresolved to exercise both scanner branches.
 */
UCLASS()
class UMissingBlueprintParentFixtureObject : public UObject
{
	GENERATED_BODY()
};
