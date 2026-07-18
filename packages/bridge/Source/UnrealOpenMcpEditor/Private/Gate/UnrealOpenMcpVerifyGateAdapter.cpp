// FUnrealOpenMcpVerifyGateAdapter implementation. See header for the bridge →
// verify glue contract.
//
// Ported from Unity Open MCP packages/bridge/Editor/Gate/VerifyGateAdapter.cs
// at adapt fidelity. P3.5 keeps the rule selection trivial — the gate runs
// every registered rule over the hint (the runner's "empty RuleIds = all
// rules" contract). When the meta-tools land in P3.6/P3.7 a real
// SelectRuleIds(paths) will narrow by extension the way Unity's does.
#include "Gate/UnrealOpenMcpVerifyGateAdapter.h"

#include "Core/CheckpointFingerprint.h"
#include "Core/IssueKey.h"
#include "Core/VerifyIssue.h"
#include "Core/VerifyResult.h"
#include "Core/VerifyRunMode.h"
#include "Core/VerifyRunner.h"
#include "Core/VerifyScope.h"

FCheckpointFingerprint FUnrealOpenMcpVerifyGateAdapter::CreateCheckpoint(const TArray<FString>& PathsHint)
{
	const FVerifyScope Scope(PathsHint);
	// Empty RuleIds → run every registered rule. P3.6 narrows this with a
	// SelectRuleIds(paths) extension map; the gate itself only needs "snapshot
	// the registered rules over the hint" for P3.5.
	return FVerifyRunner::CreateCheckpoint(Scope, TArray<FString>());
}

FVerifyResult FUnrealOpenMcpVerifyGateAdapter::ValidatePaths(const TArray<FString>& PathsHint)
{
	const FVerifyScope Scope(PathsHint);
	return FVerifyRunner::RunScoped(Scope, TArray<FString>(), EVerifyRunMode::Validate);
}

FUnrealOpenMcpGateDelta FUnrealOpenMcpVerifyGateAdapter::ComputeDelta(
	const FCheckpointFingerprint& Checkpoint,
	const FVerifyResult& Validation)
{
	// Set-difference over canonical FIssueKey strings. Mirrors Unity's
	// ComputeDelta byte-for-byte (the contract is shared with every consumer
	// of the gate: validate_edit's delta meta-tool, the apply_fix runner's
	// rollback decision, and the envelope's gate.delta field).
	TSet<FString> BeforeKeys;
	for (const auto& Pair : Checkpoint.Fingerprints)
	{
		for (const FString& Key : Pair.Value.IssueKeys)
		{
			// Skip a malformed key rather than asserting — verify's own
			// FIssueKey::TryParse is the authority and a malformed key here
			// would otherwise crash the gate mid-dispatch.
			if (FIssueKey::ValidateKey(Key))
			{
				BeforeKeys.Add(Key);
			}
		}
	}

	TSet<FString> AfterKeys;
	for (const FVerifyIssue& Issue : Validation.Issues)
	{
		AfterKeys.Add(FIssueKey::Build(Issue));
	}

	TSet<FString> NewKeys = AfterKeys.Difference(BeforeKeys);
	TSet<FString> ResolvedKeys = BeforeKeys.Difference(AfterKeys);

	FUnrealOpenMcpGateDelta Delta;
	for (const FVerifyIssue& Issue : Validation.Issues)
	{
		const FString Key = FIssueKey::Build(Issue);
		if (NewKeys.Contains(Key))
		{
			if (Issue.Severity == EVerifySeverity::Error)
			{
				++Delta.NewErrors;
			}
			else
			{
				++Delta.NewWarnings;
			}
		}
	}

	for (const FString& Key : ResolvedKeys)
	{
		// Parse the key back for the severity token (second pipe-separated
		// part). Malformed keys are skipped — they cannot be reported as
		// resolved if they cannot be parsed.
		FString RuleId, AssetPath, IssueCode;
		EVerifySeverity Severity = EVerifySeverity::Warning;
		if (FIssueKey::TryParse(Key, RuleId, Severity, AssetPath, IssueCode))
		{
			if (Severity == EVerifySeverity::Error)
			{
				++Delta.ResolvedErrors;
			}
			else
			{
				++Delta.ResolvedWarnings;
			}
		}
	}

	// TSet iteration order is not deterministic across runs; the key lists
	// surface in `gate.delta.newIssueKeys` / `resolvedIssueKeys` so the agent
	// gets a stable set. Order does not matter for the delta decision (only
	// counts do), but a sorted list is friendlier for diff tools.
	Delta.NewIssueKeys.Reserve(NewKeys.Num());
	for (const FString& Key : NewKeys)
	{
		Delta.NewIssueKeys.Add(Key);
	}
	Delta.ResolvedIssueKeys.Reserve(ResolvedKeys.Num());
	for (const FString& Key : ResolvedKeys)
	{
		Delta.ResolvedIssueKeys.Add(Key);
	}
	Delta.NewIssueKeys.Sort();
	Delta.ResolvedIssueKeys.Sort();

	return Delta;
}
