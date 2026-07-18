// FUnrealOpenMcpVerifyGateAdapter implementation. See header for the bridge →
// verify glue contract.
//
// Ported from Unity Open MCP packages/bridge/Editor/Gate/VerifyGateAdapter.cs
// at adapt fidelity. P3.5 shipped the minimal surface (CreateCheckpoint +
// ValidatePaths + ComputeDelta). P3.6 adds the rule-selection + filter
// surface the gate meta-tools need (SelectRuleIds / ResolveRuleIds /
// ValidateFiltered + the rule-id-bearing CreateCheckpoint / ValidatePaths
// overloads) — the Unity adapter's full surface, ported.
#include "Gate/UnrealOpenMcpVerifyGateAdapter.h"

#include "Core/CheckpointFingerprint.h"
#include "Core/IssueKey.h"
#include "Core/VerifyIssue.h"
#include "Core/VerifyResult.h"
#include "Core/VerifyRunMode.h"
#include "Core/VerifyRunner.h"
#include "Core/VerifyScope.h"

// P3.6 — SelectRuleIds narrows by file extension via FPaths::GetExtension.
#include "Misc/Paths.h"

namespace
{
	// Build the fallback rule roster. Mirrors Unity's
	// `private static readonly string[] FallbackRuleIds` initializer, but the
	// Unreal v1 set is the three registered rule families (broken_soft_
	// references / missing_blueprint_parent / compile_errors) rather than
	// Unity's missing_references / dependencies. Built lazily on first access
	// (rather than as a static field initializer) so the order of this TU's
	// static init vs. the verify module's static init is irrelevant.
	const TArray<FString>& BuildFallbackRuleIds()
	{
		static const TArray<FString> Fallback = {
			TEXT("broken_soft_references"),
			TEXT("missing_blueprint_parent"),
			TEXT("compile_errors"),
		};
		return Fallback;
	}
} // namespace

const TArray<FString>& FUnrealOpenMcpVerifyGateAdapter::FallbackRuleIds()
{
	return BuildFallbackRuleIds();
}

FCheckpointFingerprint FUnrealOpenMcpVerifyGateAdapter::CreateCheckpoint(const TArray<FString>& PathsHint)
{
	const FVerifyScope Scope(PathsHint);
	// Empty RuleIds → run every registered rule. The gate's P3.5 contract is
	// "snapshot the registered rules over the hint"; P3.6 narrows this via the
	// rule-id-bearing overload when checkpoint_create supplies categories.
	return FVerifyRunner::CreateCheckpoint(Scope, TArray<FString>());
}

FCheckpointFingerprint FUnrealOpenMcpVerifyGateAdapter::CreateCheckpoint(const TArray<FString>& PathsHint, const TArray<FString>& RuleIds)
{
	const FVerifyScope Scope(PathsHint);
	// Explicit RuleIds win; an empty RuleIds falls back to SelectRuleIds so a
	// caller that omitted categories still gets an extension-narrowed snapshot
	// rather than "every registered rule" (Unity parity).
	const TArray<FString> Resolved = RuleIds.Num() > 0 ? RuleIds : SelectRuleIds(PathsHint);
	return FVerifyRunner::CreateCheckpoint(Scope, Resolved);
}

FVerifyResult FUnrealOpenMcpVerifyGateAdapter::ValidatePaths(const TArray<FString>& PathsHint)
{
	const FVerifyScope Scope(PathsHint);
	return FVerifyRunner::RunScoped(Scope, TArray<FString>(), EVerifyRunMode::Validate);
}

FVerifyResult FUnrealOpenMcpVerifyGateAdapter::ValidatePaths(const TArray<FString>& PathsHint, const TArray<FString>& RuleIds)
{
	const FVerifyScope Scope(PathsHint);
	const TArray<FString> Resolved = RuleIds.Num() > 0 ? RuleIds : SelectRuleIds(PathsHint);
	return FVerifyRunner::RunScoped(Scope, Resolved, EVerifyRunMode::Validate);
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

TArray<FString> FUnrealOpenMcpVerifyGateAdapter::SelectRuleIds(const TArray<FString>& Paths)
{
	// Mirrors Unity's SelectRuleIds structure: walk paths, seed ruleSet from
	// the extension, fall back to FallbackRuleIds when nothing recognizable
	// was seen. The Unreal extension map is narrower (Unity covers 7 asset
	// types; Unreal v1 has content paths + source paths), and it grows as new
	// rule families land in verify.
	if (Paths.Num() == 0)
	{
		return FallbackRuleIds();
	}

	TSet<FString> RuleSet;
	bool bHasKnownExtension = false;

	for (const FString& Path : Paths)
	{
		// FPaths::ExtractExtension is case-insensitive on the dot but returns
		// the extension verbatim — lower-case it so .UASSET and .uasset map to
		// the same rule set.
			FString Ext = FPaths::GetExtension(Path, /*bIncludeDot=*/false);
		Ext.ToLowerInline();

		if (Ext == TEXT("uasset") || Ext == TEXT("umap"))
		{
			// Every content path may carry any of the three v1 rule families:
			// broken soft object refs, a Blueprint with a missing parent, or
			// (for Blueprint assets) compile errors. Source-path compile rules
			// do not apply here.
			RuleSet.Add(TEXT("broken_soft_references"));
			RuleSet.Add(TEXT("missing_blueprint_parent"));
			RuleSet.Add(TEXT("compile_errors"));
			bHasKnownExtension = true;
		}
		else if (Ext == TEXT("cpp") || Ext == TEXT("h") || Ext == TEXT("cs"))
		{
			// Source files — only compile_errors applies. (.cs is a courtesy
			// for an MCP-server-side source path an agent might pass; Unreal
			// itself does not compile .cs, but the rule's status provider may
			// surface editor-build compile output that mentions them.)
			RuleSet.Add(TEXT("compile_errors"));
			bHasKnownExtension = true;
		}
	}

	if (!bHasKnownExtension || RuleSet.Num() == 0)
	{
		return FallbackRuleIds();
	}

	TArray<FString> Out;
	Out.Reserve(RuleSet.Num());
	for (const FString& Id : RuleSet)
	{
		Out.Add(Id);
	}
	// Sort for deterministic `rulesApplied` output (TSet iteration order is
	// not stable across runs).
	Out.Sort();
	return Out;
}

TOptional<TArray<FString>> FUnrealOpenMcpVerifyGateAdapter::ResolveRuleIds(
	const TArray<FString>& Paths,
	const TArray<FString>& RuleIds,
	const TArray<FString>& IncludeRules,
	const TArray<FString>& ExcludeRules)
{
	// Mirrors Unity's ResolveRuleIds:
	//   requested = ruleIds ?? SelectRuleIds(paths)
	//   if (includeRules) requested = (ruleIds != null) ? Intersect : Union
	//   if (excludeRules) requested -= excludeRules
	//   return requested.Count == 0 ? null : requested.ToArray()
	//
	// The "return null" sentinel is the contract tools check to short-circuit
	// with an empty result; here it is surfaced as an unset TOptional.

	TSet<FString> Requested;
	const bool bHasExplicitRuleIds = RuleIds.Num() > 0;
	if (bHasExplicitRuleIds)
	{
		for (const FString& Id : RuleIds)
		{
			Requested.Add(Id);
		}
	}
	else
	{
		for (const FString& Id : SelectRuleIds(Paths))
		{
			Requested.Add(Id);
		}
	}

	if (IncludeRules.Num() > 0)
	{
		// Build the include set once.
		TSet<FString> Include;
		for (const FString& Id : IncludeRules)
		{
			Include.Add(Id);
		}
		if (bHasExplicitRuleIds)
		{
			// Explicit list + includeRules: keep only rules that appear in
			// BOTH (narrowing). Lets an agent pin the gate to a subset without
			// re-listing every auto-selected rule. Manual intersection
			// (TSet::Intersect returns a new set, but the in-place form keeps
			// the code explicit and matches Unity's HashSet.IntersectWith).
			for (auto It = Requested.CreateIterator(); It; ++It)
			{
				if (!Include.Contains(*It))
				{
					It.RemoveCurrent();
				}
			}
		}
		else
		{
			// No explicit list: includeRules is an additive allow-list on top
			// of the auto-selected set. Manual union (mirrors HashSet.
			// UnionWith) — add every Id in Include that is not already
			// present.
			for (const FString& Id : Include)
			{
				Requested.Add(Id);
			}
		}
	}

	if (ExcludeRules.Num() > 0)
	{
		for (const FString& Id : ExcludeRules)
		{
			Requested.Remove(Id);
		}
	}

	// Sentinel: empty after filter — distinct from "caller asked for
	// everything" (unset). Tools check this to avoid running all rules.
	if (Requested.Num() == 0)
	{
		return TOptional<TArray<FString>>();
	}

	TArray<FString> Out;
	Out.Reserve(Requested.Num());
	for (const FString& Id : Requested)
	{
		Out.Add(Id);
	}
	Out.Sort();
	return Out;
}

FUnrealOpenMcpFilteredVerifyResult FUnrealOpenMcpVerifyGateAdapter::ValidateFiltered(
	const TArray<FString>& Paths,
	const TArray<FString>& RuleIds,
	const TArray<FString>& IncludeRules,
	const TArray<FString>& ExcludeRules)
{
	const TOptional<TArray<FString>> Effective = ResolveRuleIds(Paths, RuleIds, IncludeRules, ExcludeRules);
	if (!Effective.IsSet())
	{
		// Filters narrowed the set to nothing — return an explicit empty
		// result rather than running every registered rule. Mirrors Unity's
		// ValidateFiltered sentinel branch.
		return FUnrealOpenMcpFilteredVerifyResult{
			FVerifyResult(TArray<FVerifyIssue>(), TArray<FString>(), 0),
			TArray<FString>()
		};
	}

	const FVerifyScope Scope(Paths);
	const FVerifyResult Result = FVerifyRunner::RunScoped(Scope, Effective.GetValue(), EVerifyRunMode::Validate);
	return FUnrealOpenMcpFilteredVerifyResult{Result, Effective.GetValue()};
}
