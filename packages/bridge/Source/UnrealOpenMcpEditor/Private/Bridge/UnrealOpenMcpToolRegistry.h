// Tool registry + dispatch result model for the bridge HTTP tool surface.
//
// Adapts Unity Open MCP's ToolDispatchResult + BridgeToolRegistry
// (packages/bridge/Editor/Bridge/ToolDispatchResult.cs,
//  packages/bridge/Editor/Bridge/Registry/BridgeToolRegistry.cs) to Unreal C++.
//
// Fidelity:
//   - ToolDispatchResult Ok/Fail factory pair — copy (Unity's exact shape:
//     {Success, Output, ErrorCode, ErrorMessage}, renamed to Unreal's bOk /
//     Output / Code / Message but the factory contract is identical).
//   - Registry — adapt. Unity discovers handlers via [BridgeTool] attribute
//     reflection; Unreal has no equivalent runtime reflection path that is
//     worth the complexity for P2.1, so the registry starts minimal/manual
//     (Register(Name, Handler)) per the P2.1 plan. Attribute-style discovery
//     is a later-phase convenience.
//
// P2.1 scope: registry holds TFunction handlers that take the raw JSON request
// body and return an FUnrealOpenMcpToolDispatchResult. Handlers run on the game
// thread (the HTTP server marshals every dispatch through the
// GameThreadDispatcher). No gate wrapping yet — P3.5 wraps mutating handlers.
#pragma once

#include "CoreMinimal.h"

/**
 * Outcome of a single tool dispatch. The HTTP layer maps this to the canonical
 * {ok, result, error} envelope via FUnrealOpenMcpBridgeEnvelope.
 *
 * Mirrors Unity's ToolDispatchResult factory pair (Ok(output) / Fail(code,
 * message)) at copy fidelity; fields renamed to Unreal conventions.
 */
struct UNREALOPENMCPEDITOR_API FUnrealOpenMcpToolDispatchResult
{
	/** True → success envelope; false → error envelope. */
	bool bOk = false;
	/** Pre-serialized JSON value for the success `result` field. Empty on
	 *  failure. The builder splices this verbatim — handlers own their output
	 *  schema. */
	FString Output;
	/** Stable machine-readable error code (e.g. "execution_error", "timeout").
	 *  Empty on success. */
	FString Code;
	/** Human-readable error message. Empty on success. */
	FString Message;

	/** Build a success result. OutputJson is the pre-serialized `result` value. */
	static FUnrealOpenMcpToolDispatchResult Ok(const FString& OutputJson = FString())
	{
		FUnrealOpenMcpToolDispatchResult R;
		R.bOk = true;
		R.Output = OutputJson;
		return R;
	}

	/** Build a failure result. */
	static FUnrealOpenMcpToolDispatchResult Fail(const FString& InCode, const FString& InMessage)
	{
		FUnrealOpenMcpToolDispatchResult R;
		R.bOk = false;
		R.Code = InCode;
		R.Message = InMessage;
		return R;
	}
};

/**
 * Tool handler signature. Takes the raw JSON request body (the POST body
 * verbatim) and returns a dispatch result. The body runs on the game thread —
 * the HTTP server marshals every dispatch through the GameThreadDispatcher, so
 * handlers may call UObject / editor APIs freely.
 *
 * The raw-body contract (rather than a parsed args struct) mirrors Unity's
 * BridgeToolRegistry.TryDispatch(toolName, body): each tool owns its own arg
 * extraction via the JsonBody helpers. This keeps the registry agnostic to the
 * per-tool schema and lets tools evolve their args without registry churn.
 */
using FUnrealOpenMcpToolHandler = TFunction<FUnrealOpenMcpToolDispatchResult(const FString& Body)>;

/**
 * Tool registry. Maps tool name → handler. Thread-safe for register/lookup
 * (the HTTP listener worker reads while the editor module registers at boot).
 *
 * Unity parity operations: Contains, TryGet, All (names). Register is the
 * Unreal-specific add path (Unity uses attribute reflection instead).
 */
class UNREALOPENMCPEDITOR_API FUnrealOpenMcpToolRegistry
{
public:
	FUnrealOpenMcpToolRegistry() = default;
	~FUnrealOpenMcpToolRegistry() = default;

	/** Not copyable — holds TFunction handlers. */
	FUnrealOpenMcpToolRegistry(const FUnrealOpenMcpToolRegistry&) = delete;
	FUnrealOpenMcpToolRegistry& operator=(const FUnrealOpenMcpToolRegistry&) = delete;

	/**
	 * Register a tool handler under a name. First registration wins — a second
	 * Register for the same name is ignored (returns false) so boot-order
	 * changes never silently swap a handler. Thread-safe.
	 *
	 * @param Name     the MCP tool name (unreal_open_mcp_*).
	 * @param Handler  the game-thread body. Captured by value.
	 * @return true if registered; false if the name was already taken.
	 */
	bool Register(const FString& Name, FUnrealOpenMcpToolHandler Handler);

	/** True when a handler is registered for Name. Thread-safe. */
	bool Contains(const FString& Name) const;

	/**
	 * Resolve a handler by name. Returns a copy of the handler (TFunction) so
	 * the caller can invoke it off the registry's lock; returns false when no
	 * handler is registered (OutHandler left unset).
	 */
	bool TryGet(const FString& Name, FUnrealOpenMcpToolHandler& OutHandler) const;

	/** All registered tool names. Thread-safe snapshot (copy). */
	TArray<FString> AllNames() const;

	/** Number of registered tools. */
	int32 Num() const;

private:
	/** Name → handler. Mutable because Contains/AllNames are const but take the
	 *  lock; the registry's logical state is the map, the lock is an
	 *  implementation detail. */
	mutable FCriticalSection Lock;
	TMap<FString, FUnrealOpenMcpToolHandler> Tools;
};
