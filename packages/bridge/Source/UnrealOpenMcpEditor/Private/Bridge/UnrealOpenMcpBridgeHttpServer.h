// Loopback HTTP bridge server with `GET /ping`.
//
// Adapts Unity Open MCP's BridgeHttpServer
// (packages/bridge/Editor/Bridge/BridgeHttpServer.cs — HttpListener + worker
// thread) to Unreal C++ via a raw accept loop + a hand-rolled HTTP/1.1
// request-line parser. An Unreal behavior reference (listed in
// specs/porting-map.md#p1-3) binds 127.0.0.1 via FTcpSocketBuilder for its own
// IPC transport — we mirror that loopback-only bind shape but serve plain HTTP
// request/response instead of that project's NDJSON framing.
//
// Why an own listener (per the P1.3 plan): FHttpServerModule binds ALL
// interfaces and relies on per-request remote-address checks. Unity's
// HttpListener binds the explicit loopback prefix — that fidelity is the
// primary DoD here ("Server does not bind to non-loopback addresses"). A
// listen socket bound to FIPv4Address(127,0,0,1) gives the same guarantee at
// the socket layer with zero per-request branching.
//
// P1.3 scope: GET /ping only.
//   - Tool dispatch (POST /tools/{name}) lands in P2.1.
//   - Instance lock + deterministic port resolver landed in P1.4. The
//     FUnrealOpenMcpInstancePortResolver (Runtime) owns the formula and the
//     override precedence; this server delegates to it and no longer carries a
//     DefaultPort constant of its own.
//
// P2.1 adds POST /tools/{name}: the server reads the request headers + body
// (the P1.3 reader only needed the request line), routes to the tool registry,
// and marshals handler execution onto the game thread via the dispatcher. The
// fair request queue keys on the X-Agent-Id header.
//
// P5.6 adds the bearer auth gate: CheckAuth runs BEFORE routing on EVERY
// endpoint (no exemptions — /ping, /tools/*, ...). The policy ("none" default
// | "required") and the expected token come from the project settings + the
// instance lock; the module sets them via SetAuth after Acquire (the token is
// minted with the bound port, so it is not known at Start time). Bind address
// is resolved through FUnrealOpenMcpBridgeBindAddress::Decide — remote bind is
// refused unless authMode is "required".
//
// Threading (see packages/bridge/AGENTS.md §Transport):
//   - The server IS an FRunnable: Run() is the accept loop, on its own thread.
//     Accepted connections are served inline on the SAME thread (HTTP/1.0, no
//     keep-alive, one request per connection). Simpler than Unity's ThreadPool
//     fan-out and fine for a low-volume editor control channel.
//   - /ping is dispatched to the game thread via the GameThreadDispatcher.
//     `connected:true` is the load-bearing readiness signal: it means the game
//     thread picked the ping up within the per-call timeout. A 503 fallback is
//     returned when the dispatch fails (game thread blocked or dispatcher shut
//     down) so the MCP server can classify the bridge as unreachable instead of
//     waiting on a hung socket.
//   - Tool dispatch (P2.1) also marshals the handler onto the game thread. The
//     handler NEVER runs on the listener thread — game-thread dispatch is a
//     pinned acceptance criterion.
//   - Never call a UObject / editor API on the listener thread.
#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"

class FSocket;
class FUnrealOpenMcpGameThreadDispatcher;
class FUnrealOpenMcpToolRegistry;
class FUnrealOpenMcpBridgeRequestQueue;
struct FUnrealOpenMcpToolMetadata;
enum class EUnrealOpenMcpGateMode : uint8;

/**
 * Loopback HTTP bridge server (P1.3: GET /ping).
 *
 * Lifecycle: constructed by FUnrealOpenMcpEditorModule::StartupModule, started
 * with a resolved port, and stopped in ShutdownModule. Start/Stop are
 * idempotent — hot reload / editor quit may call them more than once. The
 * server owns no UObject state and routes every UObject touch through the
 * injected dispatcher.
 */
class UNREALOPENMCPEDITOR_API FUnrealOpenMcpBridgeHttpServer : public FRunnable
{
public:
	/**
	 * Resolve the bind port from the environment. Public + static so the spec
	 * can pin the env precedence without spinning a server. Delegates to
	 * FUnrealOpenMcpInstancePortResolver (the deterministic-hash source of
	 * truth) so the formula is shared across the bridge + the TS mirror.
	 *
	 * Precedence (Unity-parity):
	 *   1. UNREAL_OPEN_MCP_BRIDGE_PORT env var (if a valid 1..65535 value)
	 *   2. -UNREAL_OPEN_MCP_BRIDGE_PORT=<n> CLI arg
	 *   3. deterministic hash of ProjectPath (20000 + sha256 % 10000)
	 *
	 * @param ProjectPath  the absolute project dir, used by the hash fallback.
	 *                     Empty → falls back to the first valid override only.
	 */
	static uint16 ResolvePort(const FString& ProjectPath);

	/** Env var name mirroring Unity's BridgeConstants.PortEnvVar. */
	static const TCHAR* PortEnvVar();

	/** True only for the loopback literal 127.0.0.1. Remote bind (0.0.0.0) is
	 *  an opt-in gated by FUnrealOpenMcpBridgeBindAddress + authMode "required". */
	static bool IsLoopbackAddress(const FString& Address);

	FUnrealOpenMcpBridgeHttpServer(
		FUnrealOpenMcpGameThreadDispatcher& InDispatcher,
		FUnrealOpenMcpToolRegistry& InToolRegistry,
		FUnrealOpenMcpBridgeRequestQueue& InRequestQueue);
	virtual ~FUnrealOpenMcpBridgeHttpServer();

	/** Not copyable — owns a listener socket + worker thread. */
	FUnrealOpenMcpBridgeHttpServer(const FUnrealOpenMcpBridgeHttpServer&) = delete;
	FUnrealOpenMcpBridgeHttpServer& operator=(const FUnrealOpenMcpBridgeHttpServer&) = delete;

	/**
	 * Bind the loopback listener and start the worker thread. Idempotent.
	 * Convenience overload for the loopback-only, no-auth case (used by specs
	 * and the default project layout). The auth gate stays in "none" mode.
	 *
	 * @param Port      explicit port (already resolved by the caller — typically
	 *                  ResolvePort()). When 0, the kernel picks an ephemeral
	 *                  port and the caller can read it back via GetPort().
	 * @param ProjectPath  surfaced verbatim in the /ping body (best-effort).
	 * @return true on a successful bind; false if the port was already in use
	 *         or the socket subsystem was unavailable. LastStartError carries
	 *         the reason.
	 */
	bool Start(uint16 Port, const FString& ProjectPath);

	/**
	 * Bind + start with an explicit bind address (loopback or remote). The bind
	 * decision is resolved through FUnrealOpenMcpBridgeBindAddress::Decide — a
	 * remote bindAddress is refused unless authMode is "required", and an
	 * invalid bindAddress coerces to loopback. The caller wires the auth policy
	 * + token via SetAuth after the instance lock is acquired (the token is
	 * minted with the bound port, so it is not known at Start time). Until
	 * SetAuth runs, the gate is in "none" mode.
	 *
	 * @return true on a successful bind; false if refused (remote without
	 *         required auth), the port was in use, or the socket subsystem was
	 *         unavailable. LastStartError carries the refusal reason.
	 */
	bool Start(uint16 Port, const FString& ProjectPath, const FString& BindAddress, const FString& AuthMode);

	/**
	 * Configure the bearer auth gate. Call AFTER the instance lock is acquired
	 * (the token is minted there) and BEFORE serving traffic. When AuthMode is
	 * "required", every request must carry `Authorization: Bearer <ExpectedToken>`
	 * or receive HTTP 401. "none" (the default) preserves localhost-trust
	 * behavior. Unknown modes fail closed (deny). Thread-safe via atomic flags
	 * — safe to call from the game thread while the worker is running.
	 *
	 * The token is never logged.
	 */
	void SetAuth(const FString& AuthMode, const FString& ExpectedToken);

	/**
	 * Stop the worker thread, close the listener, and join. Idempotent and
	 * safe to call from the game thread during ShutdownModule. After Stop
	 * returns, the server can be Start()ed again (hot reload path).
	 */
	void Stop();

	// FRunnable interface — driven by FRunnableThread.
	virtual uint32 Run() override;
	virtual void Stop() override { RequestStop(); }

	/** True between a successful Start and Stop. Read-only diagnostic. */
	bool IsRunning() const { return bRunning; }

	/** The actual bound port (may differ from the requested one when 0 was
	 *  passed — read back from the socket). 0 when not started. */
	uint16 GetPort() const { return BoundPort; }

	/** The last error from Start, if any. Cleared on each Start attempt. */
	const FString& GetLastStartError() const { return LastStartError; }

private:
	/** Mark the worker thread for shutdown (the public FRunnable Stop()
	 *  forwards here; named RequestStop to keep a single implementation). */
	void RequestStop();

	/** Close any sockets we still own. Called from Stop / dtor. */
	void CloseListener();

	/** Serve a single accepted connection: read one request, route, write one
	 *  response, close. HTTP/1.0 no keep-alive. */
	void HandleConnection(FSocket* Client);

	/**
	 * Auth gate. Runs BEFORE routing on every request (no endpoint is exempt).
	 * Returns true when the request may proceed; when it returns false the 401
	 * response has already been written. Pure decision lives in
	 * FUnrealOpenMcpBridgeAuthCheck so it is unit-testable in isolation.
	 */
	bool CheckAuth(const FString& AuthorizationHeader, FSocket& Client);

	/** Route a parsed method + path. Dispatches to the /ping handler, the tool
	 *  dispatch handler, or writes the right error body (404 / 405). */
	void RouteRequest(const FString& Method, const FString& Path, const FString& AgentId, const TArray<uint8>& Body, FSocket& Client);

	/** Build the /ping payload on the game thread via the dispatcher and write
	 *  the 200 or 503 response. */
	void HandlePing(FSocket& Client);

	/**
	 * Handle POST /tools/{name}. Reads the request body, resolves the handler
	 * from the registry, marshals it onto the game thread via the dispatcher,
	 * and writes the canonical {ok,result,error} envelope (HTTP 200) or the
	 * tool_not_found body (HTTP 404). Captures the X-Agent-Id header for the
	 * fair queue.
	 */
	void HandleToolDispatch(FSocket& Client, const FString& ToolName, const FString& Body, const FString& AgentId);

	/** Write a status line + JSON body with the standard JSON content-type
	 *  header, then close the socket. UTF-8 conversion happens here. */
	static void SendJson(FSocket& Client, uint16 StatusCode, const FString& Body);

	/** UTF-8 write helper. Returns false on a socket write failure (peer
	 *  closed); callers ignore — we close regardless. */
	static bool WriteAll(FSocket& Client, const uint8* Data, int32 Size);

	/** Read the full HTTP request (request line + headers + optional body) from
	 *  Client. Parses the method, path, and the small set of headers the bridge
	 *  needs (Content-Length, X-Agent-Id, Authorization) and returns the raw
	 *  body bytes. Returns false if no complete request arrived within the
	 *  receive window. Serves both /ping (no body) and /tools/{name} (with body). */
	bool ReadRequest(
		FSocket& Client,
		FString& OutMethod,
		FString& OutPath,
		FString& OutAgentId,
		FString& OutAuthorization,
		TArray<uint8>& OutBody);

	/** Dispatcher reference (not owned — owned by FUnrealOpenMcpEditorModule).
	 *  Every UObject touch in the /ping + tool paths goes through this. */
	FUnrealOpenMcpGameThreadDispatcher& Dispatcher;

	/** Tool registry (not owned — owned by FUnrealOpenMcpEditorModule). */
	FUnrealOpenMcpToolRegistry& ToolRegistry;

	/** Fair request queue (not owned — owned by FUnrealOpenMcpEditorModule). */
	FUnrealOpenMcpBridgeRequestQueue& RequestQueue;

	/** Bound loopback listener. nullptr before Start / after Stop. */
	FSocket* ListenSocket = nullptr;
	/** Worker thread for Run() (the accept loop). */
	FRunnableThread* Thread = nullptr;

	/** Project path surfaced in the /ping body. Captured at Start time. */
	FString ProjectPathForPing;

	/** Resolved port from Start (or the kernel-assigned one when 0 was
	 *  requested). */
	uint16 BoundPort = 0;

	/** Set in Start, cleared in Stop. Read by the worker thread and the public
	 *  IsRunning diagnostic. */
	FThreadSafeBool bRunning;

	/** Set by RequestStop to break the worker loop. */
	FThreadSafeBool bStopRequested;

	/** Last Start failure message (bind refusal, subsystem missing). */
	FString LastStartError;

	/** P5.6 auth gate state. Set via SetAuth (after the instance lock mints the
	 *  token). The worker thread reads these on every request — FString writes
	 *  are not atomic, so the module calls SetAuth from the game thread BEFORE
	 *  the first request can arrive (the lock is acquired in StartupModule
	 *  immediately after Start succeeds). Until SetAuth runs, the defaults
	 *  ("none" / empty) preserve the open localhost behavior. */
	FString AuthMode = TEXT("none");
	FString ExpectedAuthToken;
};
