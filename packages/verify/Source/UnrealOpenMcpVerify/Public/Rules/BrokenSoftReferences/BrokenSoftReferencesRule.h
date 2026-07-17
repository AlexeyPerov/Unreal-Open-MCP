// FBrokenSoftReferencesRule — greenfield verify rule that surfaces dangling
// soft object references (TSoftObjectPtr / FSoftObjectPath property values
// that point at packages the Asset Registry cannot resolve).
//
// Ported conceptually from Unity Open MCP's MissingReferences rule
// (packages/verify/Editor/Rules/MissingReferences/MissingReferencesRule.cs)
// but the scanner is greenfield: Unity walks serialized YAML GUIDs, Unreal
// walks loaded UObjects' FSoftObjectPath properties and asks the Asset
// Registry whether each target resolves. See BrokenSoftReferencesScanner.cpp
// for the deliberate-delta rationale.
//
// The rule owns its own ISoftPathResolver so:
//   - RegisterDefaults() wires the production AssetRegistry resolver
//   - tests inject a fake resolver to exercise the rule against synthetic
//     resolution maps without authoring .uasset fixtures
//
// Per packages/verify/AGENTS.md §Verify rules:
//   - GetId() is the stable "broken_soft_references"
//   - every FVerifyIssue carries IssueCode "broken_soft_reference"
//   - Scan() appends only to the sink (the runner swallows exceptions)
#pragma once

#include "CoreMinimal.h"

#include "Core/IVerifyRule.h"
#include "Rules/BrokenSoftReferences/ISoftPathResolver.h"

#include "Templates/UniquePtr.h"

/**
 * Surfaces unresolved soft object references as broken_soft_reference errors.
 */
class UNREALOPENMCPVERIFY_API FBrokenSoftReferencesRule : public IVerifyRule
{
public:
	/**
	 * Construct with a custom resolver (tests). The rule holds the resolver
	 * by ownership so a fake can outlive the rule's Scan() calls.
	 */
	explicit FBrokenSoftReferencesRule(TUniquePtr<ISoftPathResolver> InResolver);

	/** Default constructor wires the production AssetRegistry resolver. */
	FBrokenSoftReferencesRule();

	virtual FString GetId() const override;
	virtual void Scan(const FVerifyScope& Scope, const EVerifyRunMode Mode, TArray<FVerifyIssue>& Sink) const override;

private:
	TUniquePtr<ISoftPathResolver> Resolver;
};
