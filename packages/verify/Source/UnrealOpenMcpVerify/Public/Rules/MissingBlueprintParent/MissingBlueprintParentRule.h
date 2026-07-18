// FMissingBlueprintParentRule — greenfield verify rule that surfaces
// Blueprints whose declared parent class no longer resolves (the Unreal
// analogue of Unity/Godot "missing script": a deleted native parent, a
// missing parent Blueprint package, or a null ParentClass after load).
//
// Ported conceptually from Unity Open MCP's MissingReferences rule
// (packages/verify/Editor/Rules/MissingReferences/MissingReferencesRule.cs)
// — the missing-script concept — but the scanner is greenfield: Unity walks
// serialized MonoBehaviour script slots in YAML; Unreal walks UBlueprint's
// ParentClass pointer and asks the IBlueprintParentResolver whether that
// parent path resolves. See MissingBlueprintParentScanner.cpp for the
// deliberate-delta rationale.
//
// The rule owns its own IBlueprintParentResolver so:
//   - RegisterDefaults() wires the production AssetRegistry resolver
//   - tests inject a fake resolver to exercise the rule against synthetic
//     resolution maps without authoring .uasset fixtures
//
// Per packages/verify/AGENTS.md §Verify rules:
//   - GetId() is the stable "missing_blueprint_parents"
//   - every FVerifyIssue carries IssueCode "missing_blueprint_parent"
//   - Scan() appends only to the sink (the runner swallows exceptions)
#pragma once

#include "CoreMinimal.h"

#include "Core/IVerifyRule.h"
#include "Rules/MissingBlueprintParent/IBlueprintParentResolver.h"

#include "Templates/UniquePtr.h"

/**
 * Surfaces unresolved Blueprint parent classes as missing_blueprint_parent
 * Errors.
 */
class UNREALOPENMCPVERIFY_API FMissingBlueprintParentRule : public IVerifyRule
{
public:
	/**
	 * Construct with a custom resolver (tests). The rule holds the resolver
	 * by ownership so a fake can outlive the rule's Scan() calls.
	 */
	explicit FMissingBlueprintParentRule(TUniquePtr<IBlueprintParentResolver> InResolver);

	/** Default constructor wires the production AssetRegistry resolver. */
	FMissingBlueprintParentRule();

	virtual FString GetId() const override;
	virtual void Scan(const FVerifyScope& Scope, const EVerifyRunMode Mode, TArray<FVerifyIssue>& Sink) const override;

private:
	TUniquePtr<IBlueprintParentResolver> Resolver;
};
