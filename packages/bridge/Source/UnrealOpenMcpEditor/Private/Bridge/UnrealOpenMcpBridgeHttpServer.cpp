// Loopback HTTP bridge server (P1.3: GET /ping).
// See header for threading + scope rationale.
#include "Bridge/UnrealOpenMcpBridgeHttpServer.h"

#include "Bridge/UnrealOpenMcpBridgeJson.h"
#include "Bridge/UnrealOpenMcpBridgeSession.h"
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
// Receive buffer for the HTTP request-line + headers.
static constexpr int32 RecvBufferSize = 8 * 1024;

const TCHAR* FUnrealOpenMcpBridgeHttpServer::PortEnvVar()
{
	// Mirrors Unity's BridgeConstants.PortEnvVar so the same env override works
	// across both bridges and the MCP server side.
	return TEXT("UNREAL_OPEN_MCP_BRIDGE_PORT");
}

uint16 FUnrealOpenMcpBridgeHttpServer::ResolvePort()
{
	// P1.3 stub of the precedence: env override > DefaultPort. The full
	// resolver (env > arg > deterministic hash of projectPath) lands in P1.4
	// and will share the IsValidPort check with the MCP server.
	//
	// Two override sources are honored, mirroring Unity:
	//   1. CLI arg  -UNREAL_OPEN_MCP_BRIDGE_PORT=<n>  (FParse::Value form)
	//   2. process env UNREAL_OPEN_MCP_BRIDGE_PORT
	// CLI wins when present; otherwise env wins; otherwise the documented
	// default.
	FString Override;

	FString CliValue;
	if (FParse::Value(FCommandLine::Get(), TEXT("UNREAL_OPEN_MCP_BRIDGE_PORT="), CliValue) && !CliValue.IsEmpty())
	{
		Override = CliValue;
	}
	else
	{
		Override = FPlatformMisc::GetEnvironmentVariable(PortEnvVar());
	}

	if (!Override.IsEmpty())
	{
		const int32 Parsed = FCString::Atoi(*Override);
		if (Parsed >= 1 && Parsed <= 65535)
		{
			return static_cast<uint16>(Parsed);
		}
		UE_LOG(
			LogUnrealOpenMcp,
			Warning,
			TEXT("[Unreal Open MCP] ignoring invalid %s='%s' (need 1..65535); falling back to default %u"),
			PortEnvVar(),
			*Override,
			static_cast<uint32>(DefaultPort));
	}
	return DefaultPort;
}

bool FUnrealOpenMcpBridgeHttpServer::IsLoopbackAddress(const FString& Address)
{
	// The bridge NEVER binds a non-loopback address in P1.3. Pinning the
	// contract here so a later remote-bind opt-in (P5.6) has one chokepoint.
	return Address == TEXT("127.0.0.1");
}

FUnrealOpenMcpBridgeHttpServer::FUnrealOpenMcpBridgeHttpServer(
	FUnrealOpenMcpGameThreadDispatcher& InDispatcher)
	: Dispatcher(InDispatcher)
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
	const bool bParsed = ReadRequestLine(*Client, Method, Path);

	if (!bParsed)
	{
		// Peer sent no complete request line within the window — close.
		Client->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client);
		return;
	}

	RouteRequest(Method, Path, *Client);

	// HTTP/1.0 no keep-alive: always close after one request.
	Client->Close();
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client);
}

bool FUnrealOpenMcpBridgeHttpServer::ReadRequestLine(FSocket& Client, FString& OutMethod, FString& OutPath)
{
	// Minimal HTTP/1.1 request-line reader. Accumulates bytes until either the
	// end of the request line (\r\n) or the timeout. We do not parse headers
	// beyond the request line — /ping needs nothing from them, and P2.1's tool
	// dispatch will grow a Content-Length reader when it actually needs a body.
	TArray<uint8> Accumulated;
	Accumulated.Reserve(RecvBufferSize);

	const double Deadline = FPlatformTime::Seconds() + RequestReadTimeoutSeconds;
	uint8 Byte[1];

	while (FPlatformTime::Seconds() < Deadline)
	{
		int32 BytesRead = 0;
		const bool bOk = Client.Recv(Byte, 1, BytesRead);
		if (!bOk || BytesRead == 0)
		{
			// Non-blocking socket: keep polling while there is time left.
			if (Client.GetConnectionState() == SCS_CONNECTION_ERROR)
			{
				return false;
			}
			FPlatformProcess::Sleep(RecvPollIntervalSeconds);
			continue;
		}

		Accumulated.Add(Byte[0]);

		// End of request line? (\r\n)
		const int32 N = Accumulated.Num();
		if (N >= 2 && Accumulated[N - 2] == '\r' && Accumulated[N - 1] == '\n')
		{
			break;
		}
		if (N >= RecvBufferSize)
		{
			// Request line too long — bail. A real /ping request is ~20 bytes.
			return false;
		}
	}

	if (Accumulated.Num() < 2)
	{
		return false;
	}

	// Convert to a UTF-8 string and strip the trailing CRLF. Request lines are
	// ASCII in practice.
	const int32 LineLen = Accumulated.Num() - 2;
	const FString RequestLine = FString(UE_PTRDIFF_TO_INT32(LineLen), reinterpret_cast<const ANSICHAR*>(Accumulated.GetData()));

	// "METHOD SP request-target SP HTTP-version" (RFC 7230 §3.1.1). We split on
	// spaces and ignore the HTTP-version token; the request-target is what we
	// route on.
	TArray<FString> Tokens;
	RequestLine.ParseIntoArrayWS(Tokens);

	if (Tokens.Num() < 2)
	{
		return false;
	}

	OutMethod = Tokens[0];
	OutPath = Tokens[1];
	return true;
}

void FUnrealOpenMcpBridgeHttpServer::RouteRequest(const FString& Method, const FString& Path, FSocket& Client)
{
	// P1.3 routing: /ping only. 405 for wrong-method /ping, 404 for everything
	// else. Mirrors Unity's HandleRequest switch (case "/ping" default branch).

	// Trim a trailing slash so /ping and /ping/ both hit the ping handler.
	FString NormalizedPath = Path;
	while (NormalizedPath.Len() > 1 && NormalizedPath.EndsWith(TEXT("/")))
	{
		NormalizedPath.LeftChop(1);
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
		case 404: Reason = TEXT("Not Found"); break;
		case 405: Reason = TEXT("Method Not Allowed"); break;
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
