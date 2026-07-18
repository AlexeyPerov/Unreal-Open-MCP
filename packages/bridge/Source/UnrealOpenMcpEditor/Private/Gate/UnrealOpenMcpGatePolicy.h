// FUnrealOpenMcpGatePolicy — the dispatch chokepoint every mutating tool
// routes through.
//
// Ported from Unity Open MCP packages/bridge/Editor/Gate/GatePolicy.cs at copy
// fidelity, adapted to Unreal C++:
//   - Unity's static `GatePolicy.Execute` becomes a static method on a
//     non-instantiable C++ struct. The semantics are byte-for-byte the Unity
//     contract: checkpoint → mutate → validate → delta, with the same outcome
//     taxonomy (Skipped / Passed / Failed / Warned / ValidateScanFailed).
//   - The mutation callback returns an FUnrealOpenMcpToolDispatchResult
//     (Unreal's ToolDispatchResult analog); Unity's mutation callback returns
//     the C# ToolDispatchResult. Field names differ (bOk vs Success) but the
//     success/fail contract is identical.
//   - Validate-scan failures (an exception thrown by VerifyRunner) surface as
//     a distinct outcome so an agent can tell "the mutation committed but the
//     post-mutation health check could not run" from "the delta reported new
//     errors". The mutation is NOT rolled back in that case — the mutation is
//     good, the scanner blew up for an unrelated reason.
//
// P3.5 scope:
//   - The gate is wired at the bridge dispatch boundary so every mutating tool
//     follows ONE policy path before any meta-tool (P3.6 validate-edit /
//     checkpoint-create / delta) lands.
//   - Read-only tools (gate Off) skip the gate entirely — they dispatch
//     directly and their outcome is Skipped (success) / Failed (handler error).
//   - paths_hint is mandatory for mutating tools. An empty hint skips the gate
//     the same way Off does — no whole-project fallback scan is permitted.
//
// The outcome taxonomy is the wire surface an agent parses (gate.outcome in the
// widened envelope); values must stay stable.
#pragma once

#include "CoreMinimal.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"

/**
 * Outcome of a gate run. Surfaced as the wire token `gate.outcome` so an agent
 * can branch on the gate decision without parsing prose. Values are stable
 * across phases — names are pinned by GatePolicySpec and the MCP-side envelope
 * unwrap.
 */
enum class EUnrealOpenMcpGateOutcome : uint8
{
	/** Gate off (or paths_hint empty); mutation ran ungated. Outcome follows
	 *  the mutation result (Skipped on success, Failed on mutation error). */
	Skipped,
	/** Mutation committed and the post-mutation delta reported no new Errors
	 *  (warnings may be present when mode != Enforce). */
	Passed,
	/** Enforce-mode delta reported new Errors, OR the mutation itself failed,
	 *  OR checkpoint/delta key validation failed. The dispatch is surfaced as
	 *  non-passing; under Enforce the agent must fix and retry. */
	Failed,
	/** Warn-mode delta reported new Errors, OR any mode produced new warnings
	 *  that surfaced as a non-passing outcome. The mutation committed; the
	 *  agent should review before proceeding. */
	Warned,
	// The mutation committed successfully, but the post-mutation validate scan
	// threw so no delta could be computed. Distinct from Failed (where the
	// delta ran and reported new errors): here the mutation is honest-to-
	// goodness committed, and the agent should run validate_edit / scan_paths
	// manually to confirm health. The gate result carries Mutation (success)
	// + AgentNextSteps recommending the manual check; GateFailed stays true so
	// the dispatch is surfaced as a non-passing outcome.
	ValidateScanFailed,
};

/**
 * Per-rule delta summary. The gate's checkpoint (before) vs validate (after)
 * issue-key diff collapsed into scalar counts + the canonical key lists. Used
 * to resolve the outcome (Errors block under Enforce, Warnings surface under
 * Warn) and to populate the `gate.delta` field in the widened envelope.
 */
struct FUnrealOpenMcpGateDelta
{
	int32 NewErrors = 0;
	int32 NewWarnings = 0;
	int32 ResolvedErrors = 0;
	int32 ResolvedWarnings = 0;
	TArray<FString> NewIssueKeys;
	TArray<FString> ResolvedIssueKeys;
};

/**
 * Result of a GatePolicy.Execute call. Carries the underlying mutation result
 * plus the gate decision and the timing/category metadata an agent needs to
 * triage a non-passing dispatch.
 *
 * Mutating tools build their dispatch response from this; read-only tools
 * (gate Off) still get a result here, but with GateRan=false and outcome
 * Skipped/Failed so the envelope builder can branch uniformly.
 */
struct FUnrealOpenMcpGateDispatchResult
{
	/** The underlying mutation result. Always populated; on ValidateScanFailed
	 *  this is the successful mutation (the scanner failure is unrelated). */
	FUnrealOpenMcpToolDispatchResult Mutation;
	/** True when the gate ran (checkpoint + validate + delta). False when the
	 *  gate was skipped (Off mode, empty paths_hint, or read-only tool). */
	bool bGateRan = false;
	/** The resolved gate outcome. */
	EUnrealOpenMcpGateOutcome Outcome = EUnrealOpenMcpGateOutcome::Skipped;
	/** Checkpoint Id from the pre-mutation snapshot (empty when gate skipped). */
	FString CheckpointId;
	/** Rule Ids that ran in the post-mutation validate pass. Empty when gate
	 *  skipped. */
	TArray<FString> CategoriesRun;
	/** Wall-clock for the pre-mutation checkpoint, ms. 0 when gate skipped. */
	int64 CheckpointDurationMs = 0;
	/** Wall-clock for the post-mutation validate, ms. 0 when gate skipped. */
	int64 ValidationDurationMs = 0;
	/** Total gate wall-clock (checkpoint + mutate + validate + delta), ms.
	 *  0 when gate skipped. */
	int64 TotalGateDurationMs = 0;
	/** The delta computed from checkpoint vs validate. Unset when gate
	 *  skipped or validate could not run. */
	TOptional<FUnrealOpenMcpGateDelta> Delta;
	/** True when the dispatch is non-passing (Failed / Warned / mutation
	 *  faulted / ValidateScanFailed). Surfaces in the envelope so an agent can
	 *  branch without re-deriving it from the outcome. */
	bool bGateFailed = false;
	/** Actionable hints an agent should consider before retrying. */
	TArray<FString> AgentNextSteps;
};

/**
 * Gate dispatch chokepoint. Static-only; no instance state.
 *
 * The single mandatory path every mutating tool follows:
 *   GatePolicy.Execute(mode, pathsHint, mutation)
 * runs the checkpoint → mutate → validate → delta sequence and resolves the
 * outcome from the delta. Read-only tools (mode == Off) and mutating tools
 * without a paths_hint skip the gate (no whole-project fallback scan).
 */
struct UNREALOPENMCPEDITOR_API FUnrealOpenMcpGatePolicy
{
private:
	FUnrealOpenMcpGatePolicy() = delete;

public:
	/** Budget (ms) for the full gate path. A run that exceeds it logs a warning
	 *  so an operator notices a checkpoint/validate cycle that grew too heavy
	 *  for the dispatch boundary. Mirrors Unity's GateBudgetMs. */
	static constexpr int64 GateBudgetMs = 2000;

	/**
	 * Resolve the outcome from the mode + delta. Pure function (no side
	 * effects) so it is unit-testable in isolation. Mirrors Unity's
	 * GatePolicy.ResolveOutcome decision matrix:
	 *   - new Errors → Failed under Enforce, Warned under Warn.
	 *   - new Warnings only → Warned under Warn, Passed otherwise.
	 *   - clean → Passed.
	 *
	 * ValidateScanFailed is NEVER produced here — it is set only by the
	 * validate-exception catch in Execute. Returning it here would leak the
	 * scanner-failure outcome into normal delta resolution.
	 */
	static void ResolveOutcome(
		EUnrealOpenMcpGateMode Mode,
		const FUnrealOpenMcpGateDelta& Delta,
		EUnrealOpenMcpGateOutcome& OutOutcome,
		bool& bOutGateFailed);

	/**
	 * Build the agentNextSteps hints for an outcome. The hints embed concrete
	 * tool names (unreal_open_mcp_validate_edit, unreal_open_mcp_apply_fix,
	 * unreal_open_mcp_find_references) so an agent reading the envelope can
	 * chain the next step without re-discovering them.
	 */
	static TArray<FString> GenerateAgentNextSteps(
		const FUnrealOpenMcpGateDelta& Delta,
		EUnrealOpenMcpGateOutcome Outcome);

	/**
	 * Run the full gate sequence around a mutation callback.
	 *
	 * @param Mode       gate mode (Off short-circuits with no checkpoint).
	 * @param PathsHint  non-empty array of content/source paths bounding the
	 *                   checkpoint + validate. An empty hint short-circuits
	 *                   the gate (no whole-project fallback scan is permitted).
	 * @param Mutation   the mutating callback (typically the tool handler).
	 *                   Runs on the calling thread (the game thread); the gate
	 *                   does not marshal it.
	 * @return the gate dispatch result with the mutation outcome + gate
	 *         decision. Always non-empty; the caller builds the envelope from
	 *         this.
	 */
	static FUnrealOpenMcpGateDispatchResult Execute(
		EUnrealOpenMcpGateMode Mode,
		const TArray<FString>& PathsHint,
		TFunctionRef<FUnrealOpenMcpToolDispatchResult()> Mutation);

	/**
	 * Parse the wire `gate` token into a mode. Mirrors Unity's ParseMode:
	 * "warn" → Warn, "off" → Off, anything else (including empty / malformed
	 * / "enforce") → Enforce. Case-sensitive (a stray "WARN" is treated as
	 * unknown and falls back to Enforce, matching the Unity contract).
	 */
	static EUnrealOpenMcpGateMode ParseMode(const FString& Mode);

	/** Wire token for an outcome (the value emitted as gate.outcome). */
	static const TCHAR* OutcomeToken(EUnrealOpenMcpGateOutcome Outcome);
};
