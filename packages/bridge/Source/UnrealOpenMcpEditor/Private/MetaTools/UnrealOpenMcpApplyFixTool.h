// unreal_open_mcp_apply_fix — fix application workflow.
//
// Ported from Unity Open MCP packages/bridge/Editor/MetaTools/ApplyFixTool.cs +
// ApplyFixGateRunner.cs at copy fidelity. The apply surface has two halves:
//
//   1. FUnrealOpenMcpApplyFixTool — the inner handler. Resolves a fix by id
//      (or lists every fix that can resolve an issue when fix_id is omitted),
//      runs a dry-run preview, or runs the provider's Apply when dry_run is
//      false. Apply refuses when no rollback snapshot is active — a non-dry-
//      run apply must run inside the gate runner wrapper so a corrupting fix
//      is auto-reverted on failure or new errors.
//
//   2. FUnrealOpenMcpApplyFixGateRunner — the outer wrapper. Predicts the
//      files the fix may touch, snapshots them via FUnrealOpenMcpFixRollback,
//      runs the apply through FUnrealOpenMcpGatePolicy::Execute (checkpoint →
//      apply → validate → delta), and on Enforce + new errors OR a mutation
//      failure restores the snapshot. The result extends
//      FUnrealOpenMcpGateDispatchResult with rolledBack / restoredPaths +
//      a rollbackDisabled flag (gate:"off" applies have no delta to check).
//
// Arg contract (mirrors Unity's ApplyFixTool.Execute):
//   {
//     "fix_id":   optional. When omitted, the response lists every fix that
//                 can resolve issue_id.
//     "issue_id": required. Canonical {RuleId}|{SEVERITY}|{AssetPath}|{IssueCode}.
//     "dry_run":  default true. Dry-run skips the gate entirely (the runner
//                 is never invoked).
//     "gate":     enforce | warn | off. Ignored for dry_run. Non-dry-run
//                 applies are always gate-runner-mediated.
//   }
//
// Result shapes:
//   Dry-run with fix_id:    { dryRun:true, fixId, issueId, assetPath,
//                              description, safe }
//   Dry-run without fix_id: { dryRun:true, issueId, availableFixIds:[...] }
//   Apply succeeded:        { dryRun:false, success:true, description,
//                              touchedPaths:[...] }
//   Apply failed:           { ok:false, error:{ code:"fix_failed", message } }
//   Unknown fix id:         { ok:false, error:{ code:"unknown_fix",
//                              availableFixIds, applicableFixIdsForIssue } }
//
// Scope: P3.7 ships the single Safe provider (clear_broken_soft_reference).
// The dispatch path supports the contract above end-to-end; adding more
// providers in later phases does not require handler changes.
#pragma once

#include "CoreMinimal.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Gate/UnrealOpenMcpGatePolicy.h"

/**
 * Apply-fix dispatch family. Two surfaces — see header preamble.
 */
namespace FUnrealOpenMcpApplyFixTool
{
	/**
	 * Inner handler. Resolves the fix, runs dry-run preview, or applies.
	 * Refuses a non-dry-run apply when no rollback snapshot is active (the
	 * FUnrealOpenMcpApplyFixGateRunner sets the ambient flag before invoking).
	 */
	FUnrealOpenMcpToolDispatchResult Execute(const FString& Body);

	/**
	 * True when FUnrealOpenMcpApplyFixGateRunner has a FixRollback snapshot
	 * active for the current dispatch. Used by Execute() to refuse a non-dry-
	 * run apply dispatched outside the runner (e.g. a direct handler call
	 * without the rollback wrapper).
	 */
	bool IsRollbackSnapshotActive();
}

/**
 * Outer wrapper: runs the inner handler through the gate path with a
 * FixRollback snapshot active, and restores the snapshot when the fix fails
 * or introduces new errors under Enforce. Mirrors Unity's
 * ApplyFixGateRunner.Execute.
 *
 * The runner returns a widened result that adds rolledBack / restoredPaths /
 * rollbackDisabled fields to the base FUnrealOpenMcpGateDispatchResult so the
 * envelope builder can surface the rollback decision to the agent.
 */
struct UNREALOPENMCPEDITOR_API FUnrealOpenMcpApplyFixGateRunnerResult
{
	/** The underlying gate dispatch result (mutation outcome + gate decision). */
	FUnrealOpenMcpGateDispatchResult Gate;
	/** True when the snapshot was restored after a failed/corrupting fix. */
	bool bRolledBack = false;
	/** Reason for the rollback (empty when bRolledBack is false). */
	FString RollbackReason;
	/** Paths the rollback restored (content-relative form, when rolled back). */
	TArray<FString> RestoredPaths;
	/** True when gate:"off" let a successful fix commit with no rollback
	 *  protection. The envelope surfaces a structured warning so the agent
	 *  can validate manually. */
	bool bRollbackDisabled = false;
};

/**
 * Gate-runner wrapper for apply_fix. See ApplyFixGateRunner in Unity.
 */
namespace FUnrealOpenMcpApplyFixGateRunner
{
	/**
	 * Run a non-dry-run apply through the gate with rollback protection.
	 * Dry-run apply_fix must short-circuit BEFORE this call (the dispatch
	 * path in HandleToolDispatch routes dry_run=true to the inner handler
	 * directly — see UnrealOpenMcpBridgeHttpServer.cpp).
	 *
	 * @param Body       raw JSON request body (forwarded to the inner handler).
	 * @param Mode       resolved gate mode (Enforce / Warn / Off).
	 * @param PathsHint  paths_hint for the gate scope. Auto-derived from
	 *                   issue_id by the dispatcher when the caller omits it.
	 */
	FUnrealOpenMcpApplyFixGateRunnerResult Execute(
		const FString& Body,
		EUnrealOpenMcpGateMode Mode,
		const TArray<FString>& PathsHint);
}

/**
 * Register the apply_fix tool with @p Registry. The tool is registered as
 * MUTATING (default gate Enforce) so the dispatch policy routes it through
 * the gate runner; the dispatcher short-circuits dry_run=true directly to
 * the inner handler (no checkpoint/validate around a preview that mutates
 * nothing).
 */
namespace FUnrealOpenMcpApplyFixMetaTools
{
	void Register(FUnrealOpenMcpToolRegistry& Registry);
}
