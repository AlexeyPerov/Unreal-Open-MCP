// Loopback HTTP bridge server (P1.3: GET /ping; P2.1: POST /tools/{name}).
// See header for threading + scope rationale.
#include "Bridge/UnrealOpenMcpBridgeHttpServer.h"

#include "Bridge/UnrealOpenMcpBridgeEnvelope.h"
#include "Bridge/UnrealOpenMcpBridgeJson.h"
#include "Bridge/UnrealOpenMcpBridgeRequestQueue.h"
#include "Bridge/UnrealOpenMcpBridgeSession.h"
#include "Bridge/UnrealOpenMcpInstancePortResolver.h"
#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Dispatch/UnrealOpenMcpGameThreadDispatcher.h"
#include "UnrealOpenMcpLog.h"

#include "HAL/RunnableThread.h"
#include "HAL/PlatformProcess.h"
#include "Misc/EngineVersion.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Common/TcpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

// HTTP/1.1 request-line timeout. Short because /ping is a readiness probe —
// clients either send the request line immediately or have already given up.
static constexpr float RequestReadTimeoutSeconds = 5.0f;
// Per-recv poll when waiting for the request line (FTcpListener uses 200ms).
static constexpr double RecvPollIntervalSeconds = 0.05;
// How long /ping waits for the game thread to drain.
static constexpr uint32 PingDispatchTimeoutMs = 5000;
// Default per-call tool dispatch timeout. The request body's optional
// timeout_ms field overrides this when present (clamped to [MinToolTimeoutMs,
// MaxToolTimeoutMs]). Mirrors Unity's BridgeRequestBody default.
static constexpr uint32 DefaultToolDispatchTimeoutMs = 30000;
static constexpr uint32 MinToolDispatchTimeoutMs = 1000;
static constexpr uint32 MaxToolDispatchTimeoutMs = 600000;
// Receive buffer for the HTTP request-line + headers + body.
static constexpr int32 RecvBufferSize = 8 * 1024;
// Cap on a single request body so a misbehaving peer can't exhaust memory.
static constexpr int32 MaxBodyBytes = 8 * 1024 * 1024;

// Extract the timeout_ms field from a tool-dispatch request body. Hand-rolled
// substring parse (no FJsonObject) — mirrors Unity's BridgeRequestBody.
// ExtractTimeoutMs: locate "timeout_ms", find the colon, skip whitespace, parse
// the signed integer, clamp to [Min, Max]. Absent/unparseable → Default.
static uint32 ExtractTimeoutMs(const FString& Body)
{
	if (Body.IsEmpty())
	{
		return DefaultToolDispatchTimeoutMs;
	}

	const FString Key = TEXT("\"timeout_ms\"");
	const int32 KeyIdx = Body.Find(*Key, ESearchCase::CaseSensitive, ESearchDir::FromStart);
	if (KeyIdx == INDEX_NONE)
	{
		return DefaultToolDispatchTimeoutMs;
	}

	const int32 ColonIdx = Body.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, KeyIdx + Key.Len());
	if (ColonIdx == INDEX_NONE)
	{
		return DefaultToolDispatchTimeoutMs;
	}

	int32 Start = ColonIdx + 1;
	// Skip whitespace.
	while (Start < Body.Len() && FChar::IsWhitespace(Body[Start]))
	{
		++Start;
	}
	// Allow an optional leading sign so negative values parse then clamp.
	int32 End = Start;
	if (End < Body.Len() && (Body[End] == TEXT('-') || Body[End] == TEXT('+')))
	{
		++End;
	}
	while (End < Body.Len() && FChar::IsDigit(Body[End]))
	{
		++End;
	}
	if (End == Start || (End == Start + 1 && (Body[Start] == TEXT('-') || Body[Start] == TEXT('+'))))
	{
		return DefaultToolDispatchTimeoutMs;
	}

	const FString NumberText = Body.Mid(Start, End - Start);
	const int64 Parsed = FCString::Atoi64(*NumberText);
	const int64 Clamped = FMath::Clamp<int64>(
		Parsed,
		static_cast<int64>(MinToolDispatchTimeoutMs),
		static_cast<int64>(MaxToolDispatchTimeoutMs));
	return static_cast<uint32>(Clamped);
}

const TCHAR* FUnrealOpenMcpBridgeHttpServer::PortEnvVar()
{
	// Mirrors Unity's BridgeConstants.PortEnvVar so the same env override works
	// across both bridges and the MCP server side.
	return TEXT("UNREAL_OPEN_MCP_BRIDGE_PORT");
}

uint16 FUnrealOpenMcpBridgeHttpServer::ResolvePort(const FString& ProjectPath)
{
	// P1.4: delegate to FUnrealOpenMcpInstancePortResolver so the port formula
	// is shared across the bridge + the TS mirror (instance-discovery.ts).
	//
	// Override sources are read here (process env + CLI arg) and handed to the
	// resolver as parsed optionals; the resolver owns IsValidPort and the hash
	// fallback so there is a single source of truth for the precedence.
	TOptional<int32> EnvPort;
	const FString EnvValue = FPlatformMisc::GetEnvironmentVariable(PortEnvVar());
	if (!EnvValue.IsEmpty())
	{
		const int32 Parsed = FCString::Atoi(*EnvValue);
		if (FUnrealOpenMcpInstancePortResolver::IsValidPort(Parsed))
		{
			EnvPort = Parsed;
		}
		else
		{
			UE_LOG(
				LogUnrealOpenMcp,
				Warning,
				TEXT("[Unreal Open MCP] ignoring invalid %s='%s' (need 1..65535)"),
				PortEnvVar(),
				*EnvValue);
		}
	}

	TOptional<int32> CliPort;
	FString CliValue;
	if (FParse::Value(FCommandLine::Get(), TEXT("UNREAL_OPEN_MCP_BRIDGE_PORT="), CliValue) && !CliValue.IsEmpty())
	{
		const int32 Parsed = FCString::Atoi(*CliValue);
		if (FUnrealOpenMcpInstancePortResolver::IsValidPort(Parsed))
		{
			CliPort = Parsed;
		}
		else
		{
			UE_LOG(
				LogUnrealOpenMcp,
				Warning,
				TEXT("[Unreal Open MCP] ignoring invalid -UNREAL_OPEN_MCP_BRIDGE_PORT='%s' (need 1..65535)"),
				*CliValue);
		}
	}

	return static_cast<uint16>(FUnrealOpenMcpInstancePortResolver::ResolvePort(ProjectPath, EnvPort, CliPort));
}

bool FUnrealOpenMcpBridgeHttpServer::IsLoopbackAddress(const FString& Address)
{
	// The bridge NEVER binds a non-loopback address in P1.3. Pinning the
	// contract here so a later remote-bind opt-in (P5.6) has one chokepoint.
	return Address == TEXT("127.0.0.1");
}

FUnrealOpenMcpBridgeHttpServer::FUnrealOpenMcpBridgeHttpServer(
	FUnrealOpenMcpGameThreadDispatcher& InDispatcher,
	FUnrealOpenMcpToolRegistry& InToolRegistry,
	FUnrealOpenMcpBridgeRequestQueue& InRequestQueue)
	: Dispatcher(InDispatcher)
	, ToolRegistry(InToolRegistry)
	, RequestQueue(InRequestQueue)
{
}

FUnrealOpenMcpBridgeHttpServer::~FUnrealOpenMcpBridgeHttpServer()
{
	// Defensive Stop() — module destruct order can race ShutdownModule. The
	// FRunnable override forwards to RequestStop, then we join.
	Stop();
}

bool FUnrealOpenMcpBridgeHttpServer::Start(uint16 Port, const FString& ProjectPath)
{
	LastStartError.Reset();
	if (bRunning)
	{
		// Idempotent — Start on an already-running server is a no-op.
		return true;
	}

	ProjectPathForPing = ProjectPath;

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (SocketSubsystem == nullptr)
	{
		LastStartError = TEXT("Socket subsystem unavailable");
		UE_LOG(LogUnrealOpenMcp, Error, TEXT("[Unreal Open MCP] bridge HTTP start refused: %s"), *LastStartError);
		return false;
	}

	// Loopback-only bind. FTcpSocketBuilder with BoundToAddress(127.0.0.1)
	// gives the same guarantee Unity's HttpListener prefix does — no remote
	// peer can ever reach this socket. P1.3 DoD: "Server does not bind to
	// non-loopback addresses."
	const FIPv4Address Loopback(127, 0, 0, 1);
	const int32 RequestedPort = (Port == 0) ? 0 : static_cast<int32>(Port);

	ListenSocket = FTcpSocketBuilder(TEXT("UnrealOpenMcpBridgeHttpListener"))
		.AsNonBlocking()
		.WithReceiveBufferSize(RecvBufferSize)
		.BoundToAddress(Loopback)
		.BoundToPort(RequestedPort)
		.Listening(8)
		.Build();

	if (ListenSocket == nullptr)
	{
		LastStartError = FString::Printf(
			TEXT("Failed to bind 127.0.0.1:%d (in use or unavailable)"),
			RequestedPort);
		UE_LOG(LogUnrealOpenMcp, Error, TEXT("[Unreal Open MCP] bridge HTTP start refused: %s"), *LastStartError);
		return false;
	}

	// Read back the actual port — kernel-assigned when 0 was requested.
	TSharedRef<FInternetAddr> LocalAddr = SocketSubsystem->CreateInternetAddr();
	ListenSocket->GetAddress(*LocalAddr);
	BoundPort = static_cast<uint16>(LocalAddr->GetPort());

	bStopRequested = false;
	bRunning = true;

	// The accept loop runs on its own thread (this is an FRunnable). Named so
	// Output Log / profiler traces clearly identify the bridge.
	Thread = FRunnableThread::Create(this, TEXT("UnrealOpenMcpBridgeHttp"), 0, TPri_Normal);

	UE_LOG(
		LogUnrealOpenMcp,
		Log,
		TEXT("[Unreal Open MCP] bridge HTTP listening on http://127.0.0.1:%u/ping (bridge version %s)"),
		static_cast<uint32>(BoundPort),
		FUnrealOpenMcpBridgeSession::GetBridgeVersion());

	return true;
}

void FUnrealOpenMcpBridgeHttpServer::RequestStop()
{
	bStopRequested = true;
}

void FUnrealOpenMcpBridgeHttpServer::CloseListener()
{
	if (ListenSocket != nullptr)
	{
		ListenSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenSocket);
		ListenSocket = nullptr;
	}
}

void FUnrealOpenMcpBridgeHttpServer::Stop()
{
	if (!bRunning && ListenSocket == nullptr && Thread == nullptr)
	{
		return;
	}

	RequestStop();

	// Join the worker thread BEFORE closing the listener. The worker polls
	// bStopRequested every RecvPollIntervalSeconds inside its Wait(ForRead),
	// so it will observe the flag and exit Run() within that window. Closing
	// the socket from this (game) thread while the worker might still be
	// inside Wait/Accept would be a cross-thread socket-destruction race.
	if (Thread != nullptr)
	{
		Thread->Wait(true);
		delete Thread;
		Thread = nullptr;
	}

	// Safe to destroy the socket now — the worker is done touching it.
	CloseListener();

	bRunning = false;

	UE_LOG(LogUnrealOpenMcp, Log, TEXT("[Unreal Open MCP] bridge HTTP server stopped"));
}

uint32 FUnrealOpenMcpBridgeHttpServer::Run()
{
	// Accept loop. One connection served per iteration; HTTP/1.0 no keep-alive.
	// Non-blocking listener + a short WaitForRead so RequestStop is observed
	// promptly during editor teardown / hot reload.
	const FTimespan AcceptWait = FTimespan::FromSeconds(RecvPollIntervalSeconds);

	while (!bStopRequested)
	{
		if (ListenSocket == nullptr)
		{
			break;
		}

		const bool bReady = ListenSocket->Wait(ESocketWaitConditions::WaitForRead, AcceptWait);
		if (!bReady)
		{
			continue;
		}

		TSharedRef<FInternetAddr> RemoteAddr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
		FSocket* Client = ListenSocket->Accept(*RemoteAddr);
		if (Client == nullptr)
		{
			continue;
		}

		// Accepted sockets do NOT inherit the listener's non-blocking mode —
		// set it explicitly so the request-line reader can poll without
		// blocking the accept loop on a slow peer.
		Client->SetNonBlocking(true);

		// Defense in depth: the listener socket is bound to 127.0.0.1, so only
		// loopback peers can ever reach Accept(). We do NOT branch on the
		// remote address here because the socket layer already guarantees it.
		// (Adding a per-request check would imply the bind surface is wider —
		// it is not.)
		HandleConnection(Client);

		// HandleConnection always closes the client socket before returning.
	}

	return 0;
}

void FUnrealOpenMcpBridgeHttpServer::HandleConnection(FSocket* Client)
{
	if (Client == nullptr)
	{
		return;
	}

	FString Method;
	FString Path;
	FString AgentId;
	TArray<uint8> Body;
	const bool bParsed = ReadRequest(*Client, Method, Path, AgentId, Body);

	if (!bParsed)
	{
		// Peer sent no complete request within the window — close.
		Client->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client);
		return;
	}

	RouteRequest(Method, Path, AgentId, Body, *Client);

	// HTTP/1.0 no keep-alive: always close after one request.
	Client->Close();
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client);
}

bool FUnrealOpenMcpBridgeHttpServer::ReadRequest(
	FSocket& Client, FString& OutMethod, FString& OutPath, FString& OutAgentId, TArray<uint8>& OutBody)
{
	// P2.1 full request reader. Accumulates bytes until the header terminator
	// (\r\n\r\n), parses the request line + the two headers the bridge needs
	// (Content-Length, X-Agent-Id), then reads the body. Serves both /ping
	// (no body — Content-Length defaults to 0) and /tools/{name} (with body).
	//
	// All recv is on a non-blocking socket with a deadline, so a stalled peer
	// can't hang the accept loop.
	TArray<uint8> Accumulated;
	Accumulated.Reserve(RecvBufferSize);

	const double Deadline = FPlatformTime::Seconds() + RequestReadTimeoutSeconds;
	uint8 Byte[1];
	int32 HeaderEnd = INDEX_NONE;

	// Phase 1: read until \r\n\r\n (end of headers).
	while (FPlatformTime::Seconds() < Deadline)
	{
		int32 BytesRead = 0;
		const bool bOk = Client.Recv(Byte, 1, BytesRead);
		if (!bOk || BytesRead == 0)
		{
			if (Client.GetConnectionState() == SCS_CONNECTION_ERROR)
			{
				return false;
			}
			FPlatformProcess::Sleep(RecvPollIntervalSeconds);
			continue;
		}

		Accumulated.Add(Byte[0]);

		const int32 N = Accumulated.Num();
		if (N >= 4 && Accumulated[N - 4] == '\r' && Accumulated[N - 3] == '\n' &&
			Accumulated[N - 2] == '\r' && Accumulated[N - 1] == '\n')
		{
			HeaderEnd = N;
			break;
		}
		if (N >= MaxBodyBytes)
		{
			return false; // headers alone exceed the cap — bail.
		}
	}

	if (HeaderEnd == INDEX_NONE)
	{
		return false; // no complete header block within the window.
	}

	// Parse the header block as ASCII. Headers are ISO-8859-1/ASCII in
	// practice; the body (read separately below) carries the UTF-8.
	const FString HeaderBlock = FString(
		UE_PTRDIFF_TO_INT32(HeaderEnd),
		reinterpret_cast<const ANSICHAR*>(Accumulated.GetData()));

	// Split into lines on CRLF. Line 0 is the request line; the rest are
	// "Name: Value" headers until the trailing blank line.
	TArray<FString> Lines;
	HeaderBlock.ParseIntoArrayLines(Lines, false);
	if (Lines.Num() < 1)
	{
		return false;
	}

	// Request line: "METHOD SP request-target SP HTTP-version".
	TArray<FString> ReqTokens;
	Lines[0].ParseIntoArrayWS(ReqTokens);
	if (ReqTokens.Num() < 2)
	{
		return false;
	}
	OutMethod = ReqTokens[0];
	OutPath = ReqTokens[1];

	// Walk the headers. Case-insensitive name match per RFC 7230 §3.2.
	int32 ContentLength = 0;
	OutAgentId.Reset();
	for (int32 i = 1; i < Lines.Num(); ++i)
	{
		const FString& Line = Lines[i];
		if (Line.IsEmpty())
		{
			continue;
		}
		const int32 ColonIdx = Line.Find(TEXT(":"), ESearchCase::CaseSensitive);
		if (ColonIdx == INDEX_NONE)
		{
			continue;
		}
		FString Name = Line.Left(ColonIdx).TrimStartAndEnd();
		FString Value = Line.Mid(ColonIdx + 1).TrimStartAndEnd();

		if (Name.Equals(TEXT("Content-Length"), ESearchCase::IgnoreCase))
		{
			ContentLength = FCString::Atoi(*Value);
		}
		else if (Name.Equals(TEXT("X-Agent-Id"), ESearchCase::IgnoreCase))
		{
			OutAgentId = Value;
		}
	}

	// Phase 2: read the body (Content-Length bytes). Bytes already buffered
	// past the header terminator count toward the body.
	OutBody.Reset();
	const int32 AlreadyBuffered = Accumulated.Num() - HeaderEnd;
	if (AlreadyBuffered > 0)
	{
		OutBody.Append(Accumulated.GetData() + HeaderEnd, AlreadyBuffered);
	}

	if (ContentLength < 0)
	{
		ContentLength = 0;
	}
	if (ContentLength > MaxBodyBytes)
	{
		return false; // refuse oversized bodies.
	}

	while (OutBody.Num() < ContentLength && FPlatformTime::Seconds() < Deadline)
	{
		int32 BytesRead = 0;
		const int32 Want = FMath::Min<int32>(ContentLength - OutBody.Num(), RecvBufferSize);
		TArray<uint8> Chunk;
		Chunk.SetNumUninitialized(Want);
		if (!Client.Recv(Chunk.GetData(), Want, BytesRead) || BytesRead == 0)
		{
			if (Client.GetConnectionState() == SCS_CONNECTION_ERROR)
			{
				return false;
			}
			FPlatformProcess::Sleep(RecvPollIntervalSeconds);
			continue;
		}
		OutBody.Append(Chunk.GetData(), BytesRead);
	}

	if (OutBody.Num() < ContentLength)
	{
		return false; // body incomplete within the window.
	}

	// Anonymous agent fallback — mirrors Unity's ExtractAgentId synthetic id
	// so hand-rolled curl traffic flows through the queue's single-agent path.
	if (OutAgentId.IsEmpty())
	{
		OutAgentId = TEXT("agent-anon");
	}

	return true;
}

void FUnrealOpenMcpBridgeHttpServer::RouteRequest(
	const FString& Method, const FString& Path, const FString& AgentId,
	const TArray<uint8>& Body, FSocket& Client)
{
	// Routing: /ping (P1.3) + /tools/{name} (P2.1). 405 for wrong-method on a
	// known path, 404 for unknown paths. Mirrors Unity's HandleRequest switch.

	// Trim trailing slashes so /ping and /ping/ both hit the ping handler.
	FString NormalizedPath = Path;
	while (NormalizedPath.Len() > 1 && NormalizedPath.EndsWith(TEXT("/")))
	{
		NormalizedPath = NormalizedPath.LeftChop(1);
	}

	if (NormalizedPath == TEXT("/ping"))
	{
		if (Method == TEXT("GET"))
		{
			HandlePing(Client);
		}
		else
		{
			SendJson(Client, 405, FUnrealOpenMcpBridgeJson::BuildErrorJson(
				TEXT("method_not_allowed"), TEXT("GET required for /ping")));
		}
		return;
	}

	// P2.1: POST /tools/{name} → tool dispatch. The tool name is the path
	// segment after /tools/. Mirrors Unity's path.StartsWith("/tools/") branch.
	if (NormalizedPath.StartsWith(TEXT("/tools/")))
	{
		FString ToolName = NormalizedPath.Mid(static_cast<int32>(FString(TEXT("/tools/")).Len()));
		// Strip a trailing slash from the tool name too (/tools/echo/ → echo).
		while (ToolName.EndsWith(TEXT("/")))
		{
			ToolName = ToolName.LeftChop(1);
		}

		if (Method != TEXT("POST"))
		{
			SendJson(Client, 405, FUnrealOpenMcpBridgeJson::BuildErrorJson(
				TEXT("method_not_allowed"), TEXT("POST required for tool endpoints")));
			return;
		}

		if (ToolName.IsEmpty())
		{
			// /tools/ with no name — route-level 404 (no tool to look up).
			SendJson(Client, 404, FUnrealOpenMcpBridgeEnvelope::BuildToolNotFound(TEXT("")));
			return;
		}

		if (!ToolRegistry.Contains(ToolName))
		{
			SendJson(Client, 404, FUnrealOpenMcpBridgeEnvelope::BuildToolNotFound(ToolName));
			return;
		}

		// Decode the body to UTF-8 FString for the handler + JSON arg extraction.
		const FString BodyText = FString(UE_PTRDIFF_TO_INT32(Body.Num()), reinterpret_cast<const ANSICHAR*>(Body.GetData()));
		HandleToolDispatch(Client, ToolName, BodyText, AgentId);
		return;
	}

	SendJson(Client, 404, FUnrealOpenMcpBridgeJson::BuildErrorJson(
		TEXT("not_found"), FString::Printf(TEXT("Unknown path: %s"), *Path)));
}

void FUnrealOpenMcpBridgeHttpServer::HandlePing(FSocket& Client)
{
	// Marshal every UObject / editor API read through the dispatcher. The ping
	// body is small, but ProjectPath / engine version may be queried through
	// FPaths / FEngineVersion which are editor-facing enough that we want the
	// dispatcher's timeout split (GameThreadBlocked vs Timeout) to surface as
	// a 503 instead of a hung socket.
	//
	// Capture the ping-relevant fields BY VALUE so the dispatched body does not
	// touch `this` — the server may be torn down (Stop → dtor) between
	// EnqueueAsync and the game thread picking the body up. The dispatcher's
	// single-completion guard means a late body is absorbed without a crash,
	// but reading through a dead `this` would still be UB.
	const FString ProjectPath = ProjectPathForPing;
	const uint16 Port = BoundPort;
	auto Body = [ProjectPath, Port]() -> FString
	{
		FUnrealOpenMcpPingPayload Payload;
		Payload.bConnected = true;
		Payload.Status = TEXT("ready");
		Payload.ProjectPath = ProjectPath;
		Payload.UnrealVersion = FEngineVersion::Current().ToString();
		Payload.BridgeVersion = FUnrealOpenMcpBridgeSession::GetBridgeVersion();
		Payload.Mode = TEXT("live");
		Payload.Port = Port;
		Payload.bCompiling = false; // stub — real compile state lands in P5.7
		Payload.bIsPlaying = false; // stub — real PIE state lands in P5.7
		return FUnrealOpenMcpBridgeJson::BuildPingJson(Payload);
	};

	TFuture<TUnrealOpenMcpDispatchResult<FString>> Future =
		Dispatcher.EnqueueAsync<FString>(MoveTemp(Body), PingDispatchTimeoutMs);
	const TUnrealOpenMcpDispatchResult<FString> Result = Future.Get();

	if (Result.Result == EUnrealOpenMcpDispatchResult::Success)
	{
		SendJson(Client, 200, Result.Value.Get(FString(TEXT("{}"))));
		return;
	}

	// Not ready: game thread blocked, dispatcher shut down, or timed out.
	// Unity pattern — return 503 with a structured fallback body so the MCP
	// server can classify the bridge as unreachable without a hung request.
	FUnrealOpenMcpPingPayload Fallback;
	Fallback.bConnected = false;
	Fallback.Status = TEXT("not_ready");
	Fallback.ProjectPath = ProjectPath;
	Fallback.UnrealVersion = FEngineVersion::Current().ToString();
	Fallback.BridgeVersion = FUnrealOpenMcpBridgeSession::GetBridgeVersion();
	Fallback.Mode = TEXT("live");
	Fallback.Port = Port;
	Fallback.bCompiling = true; // implies "the game thread is not responsive"
	Fallback.bIsPlaying = false;

	const FString BodyJson = FUnrealOpenMcpBridgeJson::BuildPingJson(Fallback);
	SendJson(Client, 503, BodyJson);
}

void FUnrealOpenMcpBridgeHttpServer::HandleToolDispatch(
	FSocket& Client, const FString& ToolName, const FString& Body, const FString& AgentId)
{
	// P2.1 tool dispatch. The handler runs on the GAME THREAD via the
	// dispatcher — this is a pinned acceptance criterion (game-thread dispatch
	// enforced). The HTTP listener worker thread blocks on the future here,
	// same shape as HandlePing, but with a tool-specific timeout.
	//
	// Timeout resolution mirrors Unity's BridgeRequestBody.ExtractTimeoutMs:
	//   request body timeout_ms → clamped to [Min, Max] → DefaultToolDispatchTimeoutMs
	// when the field is absent. Hand-rolled substring parse (no FJsonObject) —
	// consistent with the rest of the bridge's JSON philosophy.
	const uint32 TimeoutMs = ExtractTimeoutMs(Body);

	// Resolve the handler out here (registry lookup is cheap and does not need
	// the game thread) so the dispatched body can capture it by value. If the
	// tool vanished between RouteRequest's Contains check and here (registry
	// mutated), fall back to tool_not_found.
	FUnrealOpenMcpToolHandler Handler;
	if (!ToolRegistry.TryGet(ToolName, Handler) || !Handler)
	{
		SendJson(Client, 404, FUnrealOpenMcpBridgeEnvelope::BuildToolNotFound(ToolName));
		return;
	}

	// Marshal the handler onto the game thread via the dispatcher — pinned
	// acceptance criterion (game-thread dispatch enforced). The dispatched body
	// captures ONLY by value (Handler, Body) — never `this` — so it is safe
	// even if the server is torn down between EnqueueAsync and the game thread
	// draining the body. The dispatcher's single-completion guard absorbs a
	// late body without a crash; reading through a dead `this` would be UB.
	//
	// The fair request queue (RequestQueue) is available on the listener thread
	// for X-Agent-Id-keyed fairness accounting; in P2.1's single-stream
	// listener every dispatch is serialized so the queue is exercised by its
	// own spec and wired here for the concurrent-dispatch path a later phase
	// adds. Capturing the agent id + tool name into the queue before dispatch
	// keeps the bookkeeping on the listener thread (where `this` is alive),
	// while the handler runs on the game thread with no `this` dependency.
	RequestQueue.Submit(AgentId, ToolName, []() -> FUnrealOpenMcpToolDispatchResult
	{
		// Bookkeeping-only entry: Submit enqueues, picks, and runs this lambda
		// inline on the listener thread. The real handler runs separately on
		// the game thread (below). This exercises the queue's fairness path on
		// every dispatch so the structure is live; when a concurrent dispatch
		// path lands, the handler will run through Submit directly.
		return FUnrealOpenMcpToolDispatchResult::Ok();
	});

	auto GameThreadBody = [Handler = MoveTemp(Handler), Body = Body]() -> FUnrealOpenMcpToolDispatchResult
	{
		return Handler(Body);
	};

	TFuture<TUnrealOpenMcpDispatchResult<FUnrealOpenMcpToolDispatchResult>> Future =
		Dispatcher.EnqueueAsync<FUnrealOpenMcpToolDispatchResult>(MoveTemp(GameThreadBody), TimeoutMs);

	const TUnrealOpenMcpDispatchResult<FUnrealOpenMcpToolDispatchResult> Result = Future.Get();

	// Map the dispatcher outcome to the canonical envelope. A Success means the
	// body ran (the handler returned its own Ok/Fail); every other outcome is a
	// dispatch-level failure mapped to a distinct error code.
	if (Result.Result == EUnrealOpenMcpDispatchResult::Success)
	{
		const FUnrealOpenMcpToolDispatchResult& ToolResult = Result.Value.Get(FUnrealOpenMcpToolDispatchResult::Fail(
			TEXT("empty_output"), TEXT("Tool returned no result.")));

		if (ToolResult.bOk)
		{
			SendJson(Client, 200, FUnrealOpenMcpBridgeEnvelope::BuildSuccess(ToolResult.Output));
		}
		else
		{
			// Tool ran and returned a structured failure (e.g. invalid_request,
			// execution_error). HTTP 200 with {ok:false} — structured tool
			// outcomes are never transport failures (mirrors Unity's envelope
			// discipline).
			SendJson(Client, 200, FUnrealOpenMcpBridgeEnvelope::BuildError(ToolResult.Code, ToolResult.Message));
		}
		return;
	}

	// Dispatch-level failure: game thread blocked, timeout, faulted body, or
	// dispatcher shutdown. Each maps to a distinct error code so an agent can
	// branch on the cause. All return HTTP 200 with {ok:false} — structured
	// outcomes ride 200, never transport-error status codes.
	switch (Result.Result)
	{
		case EUnrealOpenMcpDispatchResult::GameThreadBlocked:
			SendJson(Client, 200, FUnrealOpenMcpBridgeEnvelope::BuildError(
				TEXT("game_thread_blocked"),
				FString::Printf(
					TEXT("Tool '%s' could not run — the Unreal game thread is blocked (a modal dialog or long editor operation)."),
					*ToolName)));
			return;
		case EUnrealOpenMcpDispatchResult::Timeout:
			SendJson(Client, 200, FUnrealOpenMcpBridgeEnvelope::BuildError(
				TEXT("timeout"),
				FString::Printf(TEXT("Tool '%s' timed out after %u ms."), *ToolName, TimeoutMs)));
			return;
		case EUnrealOpenMcpDispatchResult::Faulted:
			SendJson(Client, 200, FUnrealOpenMcpBridgeEnvelope::BuildError(
				TEXT("execution_error"),
				Result.Message.IsEmpty()
					? FString::Printf(TEXT("Tool '%s' faulted during execution."), *ToolName)
					: Result.Message));
			return;
		case EUnrealOpenMcpDispatchResult::DispatcherShutdown:
			SendJson(Client, 200, FUnrealOpenMcpBridgeEnvelope::BuildError(
				TEXT("dispatcher_shutdown"),
				FString::Printf(
					TEXT("Tool '%s' could not run — the game-thread dispatcher is shutting down (editor teardown)."),
					*ToolName)));
			return;
		default:
			// Unknown dispatcher state — internal bridge error (HTTP 500).
			SendJson(Client, 500, FUnrealOpenMcpBridgeJson::BuildErrorJson(
				TEXT("bridge_internal_error"),
				TEXT("Unhandled dispatch state for tool.")));
			return;
	}
}

bool FUnrealOpenMcpBridgeHttpServer::WriteAll(FSocket& Client, const uint8* Data, int32 Size)
{
	int32 Sent = 0;
	while (Sent < Size)
	{
		int32 JustSent = 0;
		if (!Client.Send(Data + Sent, Size - Sent, JustSent))
		{
			return false;
		}
		if (JustSent <= 0)
		{
			// Non-blocking socket would spin here; pace it.
			FPlatformProcess::Sleep(0.005f);
			continue;
		}
		Sent += JustSent;
	}
	return true;
}

void FUnrealOpenMcpBridgeHttpServer::SendJson(FSocket& Client, uint16 StatusCode, const FString& Body)
{
	// Build the HTTP/1.1 status line + headers + body as one UTF-8 buffer so
	// the entire response is a single WriteAll call (or as few as the socket
	// needs). Content-Length matches the UTF-8 byte count, NOT the TCHAR count.
	const FTCHARToUTF8 BodyUtf8(*Body);
	const int32 BodyBytes = BodyUtf8.Length();

	FString Reason;
	switch (StatusCode)
	{
		case 200: Reason = TEXT("OK"); break;
		case 400: Reason = TEXT("Bad Request"); break;
		case 404: Reason = TEXT("Not Found"); break;
		case 405: Reason = TEXT("Method Not Allowed"); break;
		case 500: Reason = TEXT("Internal Server Error"); break;
		case 503: Reason = TEXT("Service Unavailable"); break;
		default:  Reason = TEXT("OK"); break;
	}

	const FString Header = FString::Printf(
		TEXT("HTTP/1.1 %u %s\r\n")
		TEXT("Content-Type: application/json; charset=utf-8\r\n")
		TEXT("Content-Length: %d\r\n")
		TEXT("Connection: close\r\n")
		TEXT("Access-Control-Allow-Origin: *\r\n")
		TEXT("\r\n"),
		static_cast<uint32>(StatusCode),
		*Reason,
		BodyBytes);

	const FTCHARToUTF8 HeaderUtf8(*Header);
	TArray<uint8> Packet;
	Packet.Reserve(HeaderUtf8.Length() + BodyBytes);
	Packet.Append(reinterpret_cast<const uint8*>(HeaderUtf8.Get()), HeaderUtf8.Length());
	Packet.Append(reinterpret_cast<const uint8*>(BodyUtf8.Get()), BodyBytes);

	WriteAll(Client, Packet.GetData(), Packet.Num());
}
