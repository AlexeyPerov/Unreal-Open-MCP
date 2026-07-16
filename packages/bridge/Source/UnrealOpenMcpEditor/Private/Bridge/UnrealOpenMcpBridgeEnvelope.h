// Tool dispatch envelope builders for the canonical {ok, result, error} shape.
//
// P2.1 introduces the tool-dispatch contract that every typed tool family in
// Phase 2+ will ride on. The envelope is deliberately simpler than Unity Open
// MCP's {mutation, gate, lifecycle, settleMs, ...} shape — gate wrapping is
// deferred to P3.5, so P2.1 ships a stable success/error pair that widens
// later without breaking the `ok` + `error.code` fields agents already parse.
//
// Adapted from Unity Open MCP's BridgeJson gate-envelope builders
// (packages/bridge/Editor/Bridge/BridgeJson.cs) at copy fidelity for the
// escape/append primitives, greenfield for the envelope shape (the {ok,result}
// split is an intentional P2.1 delta — see specs/execution/P2/P2.1.md
// "Intentional deltas from Unity").
//
// Wire shapes (HTTP 200 for BOTH — structured outcomes are never transport
// failures; only tool_not_found / method errors use 4xx):
//   Success: {"ok":true,"result":<result-json-value>}
//   Failure: {"ok":false,"error":{"code":"...","message":"..."}}
//
// `result` is a raw JSON value (object, array, string, number, bool, null) —
// the caller supplies pre-serialized JSON text so the builder does not need to
// understand the tool's output schema. An empty result is emitted as `null`.
#pragma once

#include "CoreMinimal.h"

/**
 * Canonical tool-dispatch envelope builders. Static-only; no instance state.
 *
 * The {ok, result, error} shape is the P2.1 contract pinned by
 * UnrealOpenMcpToolDispatchSpec and the MCP-side live-client postTool tests.
 * Field names are stable across phases: P3.5 will ADD gate/mutation fields but
 * will not rename ok / error.code / result.
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
	 * Build the failure envelope with a structured error code + message. Used
	 * for every in-band tool failure (timeout, execution_error,
	 * game_thread_blocked, body-faulted). HTTP 200 — the caller distinguishes
	 * success from failure via `ok`, not the HTTP status.
	 */
	static FString BuildError(const FString& Code, const FString& Message);

	/**
	 * Build the HTTP-level tool_not_found body (HTTP 404). Distinct from
	 * BuildError because tool_not_found is a routing failure (the tool does not
	 * exist), not a dispatch outcome (the tool ran and failed). Mirrors Unity's
	 * BridgeHttpResponse.SendToolNotFound.
	 */
	static FString BuildToolNotFound(const FString& ToolName);
};
