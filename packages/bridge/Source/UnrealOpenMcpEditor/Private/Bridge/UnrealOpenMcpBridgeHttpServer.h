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
//   - Bearer auth (P5.6) is a later opt-in; not enforced here.
//   - Instance lock + deterministic port resolver land in P1.4 — Start() takes
//     a port resolved by the caller (env override > documented default today;
//     the deterministic hash lands in P1.4).
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
//   - Never call a UObject / editor API on the listener thread.
#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"

class FSocket;
class FUnrealOpenMcpGameThreadDispatcher;

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
	/** Bind port precedence (P1.3 stub — full resolver lands in P1.4):
	 *  env `UNREAL_OPEN_MCP_BRIDGE_PORT` if a valid 1..65535 value, else the
	 *  DefaultPort constant. */
	static constexpr uint16 DefaultPort = 21111;

	/** Env var name mirroring Unity's BridgeConstants.PortEnvVar. */
	static const TCHAR* PortEnvVar();

	/**
	 * Resolve the bind port from the environment. Public + static so the P1.3
	 * spec can pin the env precedence without spinning a server.
	 * Returns DefaultPort when the env var is unset or invalid.
	 */
	static uint16 ResolvePort();

	/** True only for the loopback literal 127.0.0.1 (the only address Start
	 *  will bind). Pinning the bind-address contract at the type level. */
	static bool IsLoopbackAddress(const FString& Address);

	FUnrealOpenMcpBridgeHttpServer(FUnrealOpenMcpGameThreadDispatcher& InDispatcher);
	virtual ~FUnrealOpenMcpBridgeHttpServer();

	/** Not copyable — owns a listener socket + worker thread. */
	FUnrealOpenMcpBridgeHttpServer(const FUnrealOpenMcpBridgeHttpServer&) = delete;
	FUnrealOpenMcpBridgeHttpServer& operator=(const FUnrealOpenMcpBridgeHttpServer&) = delete;

	/**
	 * Bind the loopback listener and start the worker thread. Idempotent.
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

	/** Route a parsed method + path. Dispatches to the /ping handler or writes
	 *  the right error body (404 / 405). */
	void RouteRequest(const FString& Method, const FString& Path, FSocket& Client);

	/** Build the /ping payload on the game thread via the dispatcher and write
	 *  the 200 or 503 response. */
	void HandlePing(FSocket& Client);

	/** Write a status line + JSON body with the standard JSON content-type
	 *  header, then close the socket. UTF-8 conversion happens here. */
	static void SendJson(FSocket& Client, uint16 StatusCode, const FString& Body);

	/** UTF-8 write helper. Returns false on a socket write failure (peer
	 *  closed); callers ignore — we close regardless. */
	static bool WriteAll(FSocket& Client, const uint8* Data, int32 Size);

	/** Read up to the request headers from Client into OutMethod / OutPath.
	 *  Returns false if no complete request line arrived within the receive
	 *  window. */
	bool ReadRequestLine(FSocket& Client, FString& OutMethod, FString& OutPath);

	/** Dispatcher reference (not owned — owned by FUnrealOpenMcpEditorModule).
	 *  Every UObject touch in the /ping path goes through this. */
	FUnrealOpenMcpGameThreadDispatcher& Dispatcher;

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
};
