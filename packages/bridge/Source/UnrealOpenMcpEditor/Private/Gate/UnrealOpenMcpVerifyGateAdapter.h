// FUnrealOpenMcpVerifyGateAdapter — the bridge → verify glue.
//
// Ported from Unity Open MCP packages/bridge/Editor/Gate/VerifyGateAdapter.cs
// at adapt fidelity. Unity's adapter wraps VerifyRunner for the gate flow
// (CreateCheckpoint / ValidatePaths / ComputeDelta); the Unreal port does the
// same against FVerifyRunner. The adapter exists so the gate policy does not
// reach into verify's internals — verify's surface is the runner + the
// CheckpointFingerprint / VerifyResult / IssueKey types, and the bridge touches
// only those.
//
// P3.5 scope: minimal surface — CreateCheckpoint + ValidatePaths +
// ComputeDelta. The Unity adapter also exposes FindReferences, ScanPaths,
// ResolveRuleIds, ValidateFiltered; those land with the meta-tools that need
// them (P3.6 validate-edit / P3.6 scan-paths / P3.7 find-references).
//
// P3.6 scope: adds the rule-selection + filter surface the gate meta-tools
// need:
//   - SelectRuleIds(paths) — extension-based rule narrowing (Unity parity; the
//     Unreal extension map keys on .uasset / .umap / .cpp / .h).
//   - ResolveRuleIds(paths, requested, include, exclude) — requested → auto-
//     select → fallback, then include (intersect or additive) and exclude
//     (deny-list). Sentinel: returns an empty optional when filters reduce the
//     set to nothing so the caller can short-circuit with an empty result.
//   - ValidateFiltered(paths, requested, include, exclude) — runs Validate
//     over the resolved rule set and returns the result + the rule Ids that
//     actually ran.
//   - CreateCheckpoint(paths, ruleIds) / ValidatePaths(paths, ruleIds)
//     overloads so checkpoint_create / delta can pin the rule set rather than
//     always running every registered rule.
//
// Rule selection: P3.5 kept the gate's path trivial — pass the gate's
// paths_hint through to the runner with an empty RuleIds array (the runner
// runs every registered rule when RuleIds is empty). P3.6 ships the real
// SelectRuleIds(paths) so the meta-tools narrow by extension the way Unity's
// does by .prefab / .unity / .cs / .mat.
#pragma once

#include "CoreMinimal.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Gate/UnrealOpenMcpGatePolicy.h"

// Forward declarations of the verify types — the bridge hard-depends on
// UnrealOpenMcpVerify (Build.cs), but the gate adapter keeps the include
// surface narrow so a verify header rename touches only this file.
struct FCheckpointFingerprint;
struct FVerifyScope;

// P3.6 ValidateFiltered returns FUnrealOpenMcpFilteredVerifyResult by value,
// and that struct carries an FVerifyResult member by value — both require the
// full FVerifyResult definition at the point of declaration, so we include
// the verify header here. The other verify types (CheckpointFingerprint,
// VerifyScope) stay forward-declared because the adapter surface only
// references them by reference.
#include "Core/VerifyResult.h"

/**
 * Bundles a VerifyResult with the effective rule set after include / exclude
 * filtering. The P3.6 meta-tools surface `rulesApplied` so agents can see
 * which rules actually ran when they combine auto-select with include/exclude
 * filters.
 *
 * Ported from Unity's FilteredVerifyResult struct (copy fidelity). Defined
 * ahead of FUnrealOpenMcpVerifyGateAdapter so the adapter's ValidateFiltered
 * method can return it by value without a forward-declaration dance.
 */
struct FUnrealOpenMcpFilteredVerifyResult
{
	/** The verify result (issues + categories run + duration + unknown/available
	 *  rule rosters). Constructed with an empty Issues array when filters
	 *  reduce the rule set to nothing. */
	FVerifyResult Result;
	/** Effective rule Ids after filtering (the set Validate actually ran over).
	 *  Empty when the filters reduced the set to nothing. */
	TArray<FString> RulesApplied;
};

/**
 * Bridge → verify glue. Static-only; no instance state.
 *
 * The gate policy consults this adapter for the three verify-side operations
 * the gate flow needs:
 *   - CreateCheckpoint(paths) — the pre-mutation snapshot.
 *   - ValidatePaths(paths) — the post-mutation scan.
 *   - ComputeDelta(checkpoint, validate) — the issue-key diff.
 *
 * P3.6 widens the surface for the gate meta-tools (validate_edit /
 * checkpoint_create / delta): SelectRuleIds / ResolveRuleIds / ValidateFiltered
 * and the rule-id-bearing CreateCheckpoint / ValidatePaths overloads.
 *
 * Verify's own exception/throw model is not used (Unreal C++ editor builds do
 * not throw by default; rules use ensure/check). When a rule's Scan() body
 * faults (a verify-side ensure), FVerifyRunner logs and skips that rule's
 * issues so the rest of the gate pass still runs. A real throw from the runner
 * bubbles up to GatePolicy.Execute's validate-scan guard, which surfaces a
 * ValidateScanFailed outcome (mutation committed; manual validate
 * recommended).
 */
struct UNREALOPENMCPEDITOR_API FUnrealOpenMcpVerifyGateAdapter
{
private:
	FUnrealOpenMcpVerifyGateAdapter() = delete;

public:
	/**
	 * Capture a pre-mutation fingerprint over the hint. Runs every registered
	 * verify rule in Checkpoint mode over the scope. Mirrors Unity's
	 * VerifyGateAdapter.CreateCheckpoint.
	 */
	static FCheckpointFingerprint CreateCheckpoint(const TArray<FString>& PathsHint);

	/**
	 * Capture a pre-mutation fingerprint over the hint with a pinned rule set.
	 * Used by the P3.6 checkpoint_create meta-tool when the caller supplies an
	 * explicit `categories` array; an empty RuleIds falls back to
	 * SelectRuleIds(paths) so an agent who omits categories still gets an
	 * extension-narrowed snapshot rather than "every registered rule".
	 */
	static FCheckpointFingerprint CreateCheckpoint(const TArray<FString>& PathsHint, const TArray<FString>& RuleIds);

	/**
	 * Run the post-mutation validate scan over the hint. Runs every registered
	 * rule in Validate mode. Mirrors Unity's VerifyGateAdapter.ValidatePaths.
	 */
	static FVerifyResult ValidatePaths(const TArray<FString>& PathsHint);

	/**
	 * Run the post-mutation validate scan over the hint with a pinned rule set.
	 * Used by the P3.6 validate_edit / delta meta-tools when the caller pins
	 * the rule set (stored checkpoint categories, explicit `categories`, etc.).
	 * An empty RuleIds falls back to SelectRuleIds(paths).
	 */
	static FVerifyResult ValidatePaths(const TArray<FString>& PathsHint, const TArray<FString>& RuleIds);

	/**
	 * Compute the issue-key delta between a checkpoint and a validate result.
	 * The delta is the gate's load-bearing signal: new Errors block under
	 * Enforce, new Warnings surface under Warn, resolved issues are reported
	 * back so the agent can see what the mutation cleaned up.
	 *
	 * Mirrors Unity's VerifyGateAdapter.ComputeDelta (set difference over
	 * canonical IssueKey strings). The key format is pinned by FIssueKey:
	 * `{RuleId}|{SEVERITY}|{AssetPath}|{IssueCode}`.
	 */
	static FUnrealOpenMcpGateDelta ComputeDelta(
		const FCheckpointFingerprint& Checkpoint,
		const FVerifyResult& Validation);

	/**
	 * Auto-select rule Ids for a set of paths based on file extension. Mirrors
	 * Unity's VerifyGateAdapter.SelectRuleIds: known extensions seed the rule
	 * set; unknown / empty paths fall back to a default rule roster so a meta-
	 * tool call with no recognizable extension still runs *something* rather
	 * than silently returning an empty result.
	 *
	 * Unreal extension map (Unity's keys on .prefab / .unity / .cs / .mat):
	 *   - .uasset / .umap → broken_soft_references + missing_blueprint_parent
	 *     + compile_errors (every content path may carry any of these).
	 *   - .cpp / .h       → compile_errors (Live Coding + UBT status).
	 * The map is intentionally narrow at P3.6 — the rule roster grows as new
	 * rules land (content_path_hygiene arrives in P3.7, etc.). Unknown
	 * extensions return FallbackRuleIds so a caller passing a project-relative
	 * path without an extension still gets a meaningful scan.
	 */
	static TArray<FString> SelectRuleIds(const TArray<FString>& Paths);

	/**
	 * Resolve the effective rule set after applying include / exclude filters.
	 * Selection order mirrors Unity's ResolveRuleIds exactly:
	 *   - explicit RuleIds → SelectRuleIds(paths) → FallbackRuleIds.
	 *   - includeRules narrows (IntersectWith) when RuleIds is set; otherwise
	 *     it is additive (UnionWith) on top of the auto-selected set.
	 *   - excludeRules always wins (deny-list).
	 *
	 * Returns an unset optional when filters reduce the set to nothing — that
	 * sentinel lets the caller short-circuit with an explicit empty result
	 * rather than falling into the runner's "empty RuleIds = run all" branch.
	 */
	static TOptional<TArray<FString>> ResolveRuleIds(
		const TArray<FString>& Paths,
		const TArray<FString>& RuleIds,
		const TArray<FString>& IncludeRules,
		const TArray<FString>& ExcludeRules);

	/**
	 * Run Validate over the resolved rule set. Returns the result plus the
	 * effective rule set used (after filtering) so the meta-tool envelope can
	 * surface `rulesApplied` to the agent. When filters reduce the set to
	 * nothing, returns an explicit empty result + empty rulesApplied (no rules
	 * run) rather than running every registered rule.
	 *
	 * Mirrors Unity's VerifyGateAdapter.ValidateFiltered at copy fidelity; the
	 * shape (result + rulesApplied) is what ValidateEditTool surfaces as
	 * `rulesApplied` in the validate_edit payload.
	 */
	static FUnrealOpenMcpFilteredVerifyResult ValidateFiltered(
		const TArray<FString>& Paths,
		const TArray<FString>& RuleIds,
		const TArray<FString>& IncludeRules,
		const TArray<FString>& ExcludeRules);

	/** Fallback rule set when paths is empty / no extension is recognized.
	 *  Mirrors Unity's FallbackRuleIds = { missing_references, dependencies }
	 *  — the Unreal v1 set is the three registered rule families so a meta-
	 *  tool call with no recognizable path still runs them. Exposed for the
	 *  spec that pins the fallback roster. */
	static const TArray<FString>& FallbackRuleIds();
};
