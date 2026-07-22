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
//
// P3.5 scope: registry carries per-tool metadata (mutating + default gate mode)
// alongside the handler. The dispatch path consults the metadata to decide
// whether to route through GatePolicy.Execute. The Register(Name, Handler)
// overload stays as a back-compat default: a non-mutating tool with gate Off
// (the safe default for tools that only read editor state — no gate path).
// Mutating tools use the Register(Name, Handler, Metadata) overload with
// bIsMutating=true so the dispatch path runs them through the gate.
//
// P5.5 scope: the dispatch result carries an OPTIONAL image payload
// (base64 bytes + media type) so the screenshot family can return a PNG
// alongside its `result` metadata. The envelope emits it as a top-level
// `image` field next to `result`; read-only image tools stay gate-free (they
// read pixels but mutate no editor/project state). The MCP LiveClient unwraps
// the `image` field into an MCP image content block so the base64 never
// surfaces as text.
#pragma once

#include "CoreMinimal.h"

/**
 * Gate mode for the dispatch policy. Mirrors Unity's GateMode (Enforce / Warn /
 * Off). Names align with the wire tokens emitted by
 * FUnrealOpenMcpGatePolicy::ParseMode so the registry metadata, the request
 * override, and the dispatcher all use one vocabulary.
 */
enum class EUnrealOpenMcpGateMode : uint8
{
	/** Validate after mutate; new Errors hard-fail the dispatch. */
	Enforce,
	/** Validate after mutate; new Errors emit a warning but commit the mutation. */
	Warn,
	/** Skip the gate entirely (read-only tools + `gate:"off"` requests). */
	Off,
};

/**
 * Per-tool metadata consulted by the dispatch policy. Pinned by the registry
 * at registration time so the dispatcher never has to infer mutability from
 * the tool name.
 *
 * Defaults are the safe-for-read-only-tools set: not mutating, gate Off. A
 * mutating tool sets bIsMutating=true; the per-tool DefaultGate is the
 * recommendation when the caller omitted `gate` from the request and no
 * project default applies (the registry default for P3.5 is Enforce).
 */
struct FUnrealOpenMcpToolMetadata
{
	/** True when the tool changes Unreal editor/project state. Mutating tools
	 *  route through FUnrealOpenMcpGatePolicy::Execute; read-only tools
	 *  dispatch directly. */
	bool bIsMutating = false;

	/** Recommended gate mode when neither the request nor the project default
	 *  applies. Catalog metadata only — request `gate` and the project default
	 *  both win over this value (mirrors Unity's precedence). */
	EUnrealOpenMcpGateMode DefaultGate = EUnrealOpenMcpGateMode::Off;

	/** Build the default (read-only) metadata. */
	static FUnrealOpenMcpToolMetadata ReadOnly()
	{
		FUnrealOpenMcpToolMetadata M;
		M.bIsMutating = false;
		M.DefaultGate = EUnrealOpenMcpGateMode::Off;
		return M;
	}

	/** Build the mutating-tool metadata with the requested default gate. */
	static FUnrealOpenMcpToolMetadata Mutating(EUnrealOpenMcpGateMode Default = EUnrealOpenMcpGateMode::Enforce)
	{
		FUnrealOpenMcpToolMetadata M;
		M.bIsMutating = true;
		M.DefaultGate = Default;
		return M;
	}
};

/**
 * Optional image payload carried alongside the success `result`. P5.5 — the
 * screenshot family returns a base64-encoded PNG as MCP image content; the
 * envelope emits this as a top-level `image` field next to `result`, and the
 * MCP LiveClient unwraps it into an MCP image content block so the base64
 * never surfaces as a text block. Empty `Base64Data` means "no image" (the
 * common case — every non-screenshot tool).
 *
 * Mirrors the Unity Open MCP inlineImage transport: same base64-without-data-
 * URI-prefix + mimeType contract, surfaced as a dedicated field rather than a
 * magic key inside `result` so the LiveClient unwraps it generically without
 * inspecting per-tool result schemas.
 */
struct UNREALOPENMCPEDITOR_API FUnrealOpenMcpImagePayload
{
	/** Base64-encoded image bytes (no `data:` URI prefix). Empty = no image. */
	FString Base64Data;
	/** MIME type (e.g. "image/png"). Defaults to "image/png" when unset. */
	FString MediaType = TEXT("image/png");

	/** True when the payload carries a non-empty base64 image. */
	bool HasImage() const { return !Base64Data.IsEmpty(); }
};

/**
 * Outcome of a single tool dispatch. The HTTP layer maps this to the canonical
 * {ok, result, error} envelope via FUnrealOpenMcpBridgeEnvelope.
 *
 * Mirrors Unity's ToolDispatchResult factory pair (Ok(output) / Fail(code,
 * message)) at copy fidelity; fields renamed to Unreal conventions.
 *
 * P5.5 adds an optional Image payload: a success result may carry a base64 PNG
 * alongside its `result` metadata. The envelope builder appends a top-level
 * `image` field when present. Failures never carry an image.
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
	/** P5.5 — optional image payload for screenshot tools. Emitted as a
	 *  top-level `image` field in the success envelope when present. Ignored
	 *  on failure (the builder never emits image on a Fail result). */
	FUnrealOpenMcpImagePayload Image;

	/** Build a success result. OutputJson is the pre-serialized `result` value. */
	static FUnrealOpenMcpToolDispatchResult Ok(const FString& OutputJson = FString())
	{
		FUnrealOpenMcpToolDispatchResult R;
		R.bOk = true;
		R.Output = OutputJson;
		return R;
	}

	/** P5.5 — build a success result carrying an image payload (base64 bytes +
	 *  media type) alongside the `result` metadata. The screenshot family uses
	 *  this so the envelope emits `{ok:true, result:<metadata>, image:{data,
	 *  mediaType}}` and the LiveClient unwraps `image` into an MCP image block. */
	static FUnrealOpenMcpToolDispatchResult OkWithImage(
		const FString& OutputJson,
		const FString& Base64Data,
		const FString& MediaType = TEXT("image/png"))
	{
		FUnrealOpenMcpToolDispatchResult R;
		R.bOk = true;
		R.Output = OutputJson;
		R.Image.Base64Data = Base64Data;
		R.Image.MediaType = MediaType;
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
 * Tool registry. Maps tool name → (handler, metadata). Thread-safe for
 * register/lookup (the HTTP listener worker reads while the editor module
 * registers at boot).
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
	 * Register a read-only tool handler under a name. Convenience overload: the
	 * full Register(name, handler, metadata) form covers mutating tools; this
	 * shorthand records the tool as non-mutating with gate Off so the dispatch
	 * path runs it directly without GatePolicy.Execute.
	 *
	 * @param Name     the MCP tool name (unreal_open_mcp_*).
	 * @param Handler  the game-thread body. Captured by value.
	 * @return true if registered; false if the name was already taken.
	 */
	bool Register(const FString& Name, FUnrealOpenMcpToolHandler Handler)
	{
		return Register(Name, MoveTemp(Handler), FUnrealOpenMcpToolMetadata::ReadOnly());
	}

	/**
	 * Register a tool handler under a name with explicit metadata. First
	 * registration wins — a second Register for the same name is ignored
	 * (returns false) so boot-order changes never silently swap a handler or
	 * its mutability/gate classification. Thread-safe.
	 *
	 * @param Name     the MCP tool name (unreal_open_mcp_*).
	 * @param Handler  the game-thread body. Captured by value.
	 * @param Metadata mutability + default gate metadata consulted by the
	 *                 dispatch policy.
	 * @return true if registered; false if the name was already taken.
	 */
	bool Register(const FString& Name, FUnrealOpenMcpToolHandler Handler, const FUnrealOpenMcpToolMetadata& Metadata);

	/** True when a handler is registered for Name. Thread-safe. */
	bool Contains(const FString& Name) const;

	/**
	 * Resolve a handler by name. Returns a copy of the handler (TFunction) so
	 * the caller can invoke it off the registry's lock; returns false when no
	 * handler is registered (OutHandler left unset).
	 */
	bool TryGet(const FString& Name, FUnrealOpenMcpToolHandler& OutHandler) const;

	/**
	 * Resolve a tool's metadata by name. Returns false when no tool is
	 * registered (OutMetadata is left as the read-only default so the dispatch
	 * path falls back to direct, ungated dispatch).
	 */
	bool TryGetMetadata(const FString& Name, FUnrealOpenMcpToolMetadata& OutMetadata) const;

	/** All registered tool names. Thread-safe snapshot (copy). */
	TArray<FString> AllNames() const;

	/** Number of registered tools. */
	int32 Num() const;

private:
	/** Name → (handler, metadata). Mutable because Contains/AllNames are const
	 *  but take the lock; the registry's logical state is the map, the lock is
	 *  an implementation detail. */
	mutable FCriticalSection Lock;

	/** Internal entry — handler + metadata travel together so the dispatch
	 *  policy never has to consult a separate metadata map. */
	struct FEntry
	{
		FUnrealOpenMcpToolHandler Handler;
		FUnrealOpenMcpToolMetadata Metadata;
	};
	TMap<FString, FEntry> Tools;
};
