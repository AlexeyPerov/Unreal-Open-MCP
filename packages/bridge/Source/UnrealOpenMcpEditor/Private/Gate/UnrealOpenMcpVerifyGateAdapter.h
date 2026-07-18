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
// Rule selection: P3.5 keeps it trivial — pass the gate's paths_hint through
// to the runner with an empty RuleIds array (the runner runs every registered
// rule when RuleIds is empty). When the meta-tools land, a real
// SelectRuleIds(paths) will narrow by extension (Unreal's .uasset / .umap /
// .cpp / .h) the way Unity's does by .prefab / .unity / .cs / .mat. For now
// the gate's contract is "checkpoint every registered rule over the hint";
// the rules themselves are responsible for ignoring irrelevant paths.
#pragma once

#include "CoreMinimal.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Gate/UnrealOpenMcpGatePolicy.h"

// Forward declarations of the verify types — the bridge hard-depends on
// UnrealOpenMcpVerify (Build.cs), but the gate adapter keeps the include
// surface narrow so a verify header rename touches only this file.
struct FCheckpointFingerprint;
struct FVerifyResult;
struct FVerifyScope;

/**
 * Bridge → verify glue. Static-only; no instance state.
 *
 * The gate policy consults this adapter for the three verify-side operations
 * the gate flow needs:
 *   - CreateCheckpoint(paths) — the pre-mutation snapshot.
 *   - ValidatePaths(paths) — the post-mutation scan.
 *   - ComputeDelta(checkpoint, validate) — the issue-key diff.
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
	 * Run the post-mutation validate scan over the hint. Runs every registered
	 * rule in Validate mode. Mirrors Unity's VerifyGateAdapter.ValidatePaths.
	 */
	static FVerifyResult ValidatePaths(const TArray<FString>& PathsHint);

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
};
