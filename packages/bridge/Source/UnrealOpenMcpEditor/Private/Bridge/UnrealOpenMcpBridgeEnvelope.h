// Tool dispatch envelope builders for the canonical {ok, result, error} shape.
//
// P2.1 introduced the tool-dispatch contract that every typed tool family in
// Phase 2+ will ride on. The P2.1 envelope was deliberately simpler than Unity
// Open MCP's {mutation, gate, lifecycle, settleMs, ...} shape — gate wrapping
// was deferred to P3.5, so P2.1 shipped a stable success/error pair.
//
// P3.5 widens the envelope toward Unity's mutation/gate shape WITHOUT dropping
// the `ok` + `error.code` fields agents already parse. The success body keeps
// `{ok:true, result:<value>}`; a mutating dispatch now ADDS a `gate` summary
// (outcome / delta / agentNextSteps) alongside result so an agent can branch
// on the gate decision. Read-only tools and structured tool failures keep the
// P2.1 shape exactly — the widening only adds fields, never renames or removes.
//
// Adapted from Unity Open MCP's BridgeJson gate-envelope builders
// (packages/bridge/Editor/Bridge/BridgeJson.cs) at copy fidelity for the
// escape/append primitives, greenfield for the envelope shape (the {ok,result}
// split is an intentional P2.1 delta — see specs/execution/P2/P2.1.md
// "Intentional deltas from Unity"; P3.5 widens toward Unity without breaking
// that contract).
//
// Wire shapes (HTTP 200 for BOTH — structured outcomes are never transport
// failures; only tool_not_found / method errors use 4xx):
//   Read-only success: {"ok":true,"result":<result-json-value>}
//   Mutating success:  {"ok":true,"result":<value>,"gate":{...}}
//   Failure:           {"ok":false,"error":{"code":"...","message":"..."}}
//
// `result` is a raw JSON value (object, array, string, number, bool, null) —
// the caller supplies pre-serialized JSON text so the builder does not need to
// understand the tool's output schema. An empty result is emitted as `null`.
#pragma once

#include "CoreMinimal.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Gate/UnrealOpenMcpGatePolicy.h"

/**
 * Canonical tool-dispatch envelope builders. Static-only; no instance state.
 *
 * The {ok, result, error} shape is the P2.1 contract pinned by
 * UnrealOpenMcpToolDispatchSpec and the MCP-side live-client postTool tests.
 * Field names are stable across phases: P3.5 ADDED gate/mutation fields but
 * does not rename ok / error.code / result.
 */
struct UNREALOPENMCPEDITOR_API FUnrealOpenMcpBridgeEnvelope
{
	/**
	 * Build the success envelope. ResultJson is spliced verbatim as the value
	 * of the `result` field — it must already be valid JSON (the builder does
	 * not validate or re-serialize it). Pass an empty string to emit `null`.
	 *
	 * Example: BuildSuccess("{\"echo\":123}") → {"ok":true,"result":{"echo":123}}
	 */
	static FString BuildSuccess(const FString& ResultJson);

	/**
	 * Build the success envelope with a gate summary. P3.5 widening — every
	 * mutating dispatch path emits this form so an agent can branch on
	 * `gate.outcome` (passed / warned / failed / skipped / validate_scan_failed)
	 * without parsing prose in agentNextSteps. The P2.1 ok/result fields stay
	 * first so a P2.1 parser keeps working.
	 *
	 * The gate summary fields are emitted in a pinned order:
	 *   gate: { ran, outcome, gateFailed, checkpointId?, delta?, categoriesRun?,
	 *           checkpointMs?, validateMs?, totalMs?, agentNextSteps? }
	 * Optional fields (marked `?`) are emitted only when the gate ran and the
	 * value is non-empty / meaningful.
	 */
	static FString BuildSuccessWithGate(
		const FString& ResultJson,
		const FUnrealOpenMcpGateDispatchResult& GateResult);

	/**
	 * Build the success envelope with a gate summary + apply_fix rollback
	 * block. P3.7 widening — apply_fix dispatches that rolled back (failed
	 * fix or new errors under enforce) OR committed with gate:"off" (no
	 * rollback protection) emit this form so an agent sees the rollback
	 * decision in a structured `rollback` field rather than parsing prose.
	 *
	 * Shape:
	 *   { ok, result, gate:{...}, rollback:{ rolledBack, reason?, restoredPaths?,
	 *                                        rollbackDisabled? } }
	 * The rollback block is emitted ONLY when bRolledBack or bRollbackDisabled
	 * is true. Optional rollback fields (`?`) are omitted when empty.
	 */
	struct FApplyFixRollbackFields
	{
		bool bRolledBack = false;
		bool bRollbackDisabled = false;
		FString RollbackReason;
		TArray<FString> RestoredPaths;
	};
	static FString BuildSuccessWithGateAndRollback(
		const FString& ResultJson,
		const FUnrealOpenMcpGateDispatchResult& GateResult,
		const FApplyFixRollbackFields& Rollback);

	/**
	 * Build the failure envelope with a structured error code + message. Used
	 * for every in-band tool failure (timeout, execution_error,
	 * game_thread_blocked, body-faulted, paths_hint_required). HTTP 200 — the
	 * caller distinguishes success from failure via `ok`, not the HTTP status.
	 *
	 * When a gate summary is available (the mutation committed but the gate
	 * surfaced ValidateScanFailed, or the dispatch was a Warned mutation), the
	 * gate block is appended so the agent sees the gate decision alongside the
	 * tool error. Pass a nullptr to emit the bare P2.1 failure shape.
	 */
	static FString BuildError(const FString& Code, const FString& Message);
	static FString BuildErrorWithGate(
		const FString& Code,
		const FString& Message,
		const FUnrealOpenMcpGateDispatchResult& GateResult);

	/**
	 * Build the HTTP-level tool_not_found body (HTTP 404). Distinct from
	 * BuildError because tool_not_found is a routing failure (the tool does not
	 * exist), not a dispatch outcome (the tool ran and failed). Mirrors Unity's
	 * BridgeHttpResponse.SendToolNotFound.
	 */
	static FString BuildToolNotFound(const FString& ToolName);

	/**
	 * Build the paths_hint_required error body. P3.5 — mutating dispatches
	 * with an empty paths_hint fail fast with this structured error BEFORE any
	 * mutation runs. The body includes the resolved gate mode so an agent
	 * reading the failure sees which default was applied.
	 */
	static FString BuildPathsHintRequired(const FString& ToolName, EUnrealOpenMcpGateMode EffectiveGate);
};
