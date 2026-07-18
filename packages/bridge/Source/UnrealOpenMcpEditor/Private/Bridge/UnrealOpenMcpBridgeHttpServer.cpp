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
#include "Gate/UnrealOpenMcpGatePolicy.h"
#include "MetaTools/UnrealOpenMcpApplyFixTool.h"
#include "UnrealOpenMcpLog.h"

#include "Core/IssueKey.h"
#include "Core/VerifyRunner.h"

#include "HAL/RunnableThread.h"
#include "HAL/PlatformProcess.h"
#include "Misc/EngineVersion.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Common/TcpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
// P3.5 — FJsonObject / TJsonReader for the paths_hint array extraction.
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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

// P3.5 — extract the wire `gate` token from the request body. Hand-rolled
// substring parse, same philosophy as ExtractTimeoutMs: a tiny scanner that
// avoids the FJsonObject dependency for the dispatch boundary. Recognizes
// "enforce" / "warn" / "off" (the wire tokens); absent/unparseable → empty so
// the caller falls back to the tool-default / project-default precedence.
static FString ExtractGateToken(const FString& Body)
{
	if (Body.IsEmpty())
	{
		return FString();
	}
	const FString Key = TEXT("\"gate\"");
	const int32 KeyIdx = Body.Find(*Key, ESearchCase::CaseSensitive, ESearchDir::FromStart);
	if (KeyIdx == INDEX_NONE)
	{
		return FString();
	}
	const int32 ColonIdx = Body.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, KeyIdx + Key.Len());
	if (ColonIdx == INDEX_NONE)
	{
		return FString();
	}
	int32 Start = ColonIdx + 1;
	while (Start < Body.Len() && FChar::IsWhitespace(Body[Start]))
	{
		++Start;
	}
	if (Start >= Body.Len() || Body[Start] != TEXT('"'))
	{
		return FString();
	}
	++Start; // step past the opening quote
	int32 End = Start;
	while (End < Body.Len() && Body[End] != TEXT('"') && Body[End] != TEXT('\\'))
	{
		++End;
	}
	if (End >= Body.Len() || Body[End] != TEXT('"'))
	{
		return FString();
	}
	return Body.Mid(Start, End - Start);
}

// P3.5 — extract the `paths_hint` string array from the request body. Parses
// the body into an FJsonObject (paths_hint is an array of strings; a hand-
// rolled scan would be brittle for arrays). Empty / absent / wrong-type →
// empty array; the dispatch policy treats an empty hint on a mutating tool as
// paths_hint_required.
static TArray<FString> ExtractPathsHint(const FString& Body)
{
	TArray<FString> Out;
	if (Body.TrimStartAndEnd().IsEmpty())
	{
		return Out;
	}
	TSharedPtr<FJsonObject> Object;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return Out;
	}
	const TArray<TSharedPtr<FJsonValue>>* Arr = Object->GetArrayField(TEXT("paths_hint"));
	if (Arr == nullptr)
	{
		return Out;
	}
	for (const TSharedPtr<FJsonValue>& Item : *Arr)
	{
		if (Item.IsValid() && Item->Type == EJson::String)
		{
			const FString Path = Item->AsString();
			if (!Path.IsEmpty())
			{
				Out.Add(Path);
			}
		}
	}
	return Out;
}

// P3.7 — extract a named boolean from the request body. Used by the apply_fix
// dispatch path to read `dry_run` (default true) BEFORE the registry resolves
// metadata, so the dry-run short-circuit can decide whether to skip the gate.
// Absent / wrong-type → DefaultValue.
static bool ExtractBoolField(const FString& Body, const FString& FieldName, bool DefaultValue)
{
	if (Body.TrimStartAndEnd().IsEmpty())
	{
		return DefaultValue;
	}
	TSharedPtr<FJsonObject> Object;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return DefaultValue;
	}
	if (!Object->HasTypedField<EJson::Bool>(FieldName))
	{
		return DefaultValue;
	}
	return Object->GetBoolField(FieldName);
}

// P3.7 — extract a named string from the request body. Used by the apply_fix
// dispatch path to read `issue_id` so the dispatcher can auto-derive a
// paths_hint (the issue's asset path) when the caller omits one. Absent /
// wrong-type → empty.
static FString ExtractStringField(const FString& Body, const FString& FieldName)
{
	if (Body.TrimStartAndEnd().IsEmpty())
	{
		return FString();
	}
	TSharedPtr<FJsonObject> Object;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return FString();
	}
	if (!Object->HasTypedField<EJson::String>(FieldName))
	{
		return FString();
	}
	return Object->GetStringField(FieldName);
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

namespace
{
	// Outcome of one tool dispatch through the gate. The dispatched game-thread
	// body returns both the tool-level result and the gate-level result so the
	// listener thread can build the right envelope (success vs failure, with vs
	// without a gate summary) without re-running the gate. Read-only tools
	// synthesize a Skipped gate result so the envelope builder branches
	// uniformly; mutating tools route through FUnrealOpenMcpGatePolicy::Execute
	// and carry the resolved gate decision.
	//
	// P3.7 — apply_fix carries an optional rollback block (rolledBack /
	// restoredPaths / rollbackDisabled) when the ApplyFixGateRunner had to
	// restore the snapshot after a failed/corrupting fix. Every other tool
	// leaves these fields at their defaults.
	struct FUnrealOpenMcpDispatchOutcome
	{
		FUnrealOpenMcpToolDispatchResult Tool;
		FUnrealOpenMcpGateDispatchResult Gate;
		// P3.7 — apply_fix-only rollback summary. Emitted as a top-level
		// `rollback` block in the success envelope when bRolledBack or
		// bRollbackDisabled is true.
		bool bRolledBack = false;
		bool bRollbackDisabled = false;
		FString RollbackReason;
		TArray<FString> RestoredPaths;
	};
} // namespace

void FUnrealOpenMcpBridgeHttpServer::HandleToolDispatch(
	FSocket& Client, const FString& ToolName, const FString& Body, const FString& AgentId)
{
	// P3.5 tool dispatch. P2.1 ran the handler directly; P3.5 wraps every
	// mutating dispatch in FUnrealOpenMcpGatePolicy::Execute so the
	// checkpoint → mutate → validate → delta policy path is the single
	// mandatory chokepoint. Read-only tools still dispatch directly (gate Off
	// skips the gate entirely). The handler runs on the GAME THREAD via the
	// dispatcher — pinned acceptance criterion (game-thread dispatch enforced).
	//
	// Timeout resolution mirrors Unity's BridgeRequestBody.ExtractTimeoutMs:
	//   request body timeout_ms → clamped to [Min, Max] → DefaultToolDispatchTimeoutMs
	// when the field is absent.
	const uint32 TimeoutMs = ExtractTimeoutMs(Body);

	// Resolve the handler + metadata out here (registry lookup is cheap and
	// does not need the game thread) so the dispatched body can capture them by
	// value. If the tool vanished between RouteRequest's Contains check and
	// here (registry mutated), fall back to tool_not_found.
	FUnrealOpenMcpToolHandler Handler;
	if (!ToolRegistry.TryGet(ToolName, Handler) || !Handler)
	{
		SendJson(Client, 404, FUnrealOpenMcpBridgeEnvelope::BuildToolNotFound(ToolName));
		return;
	}
	FUnrealOpenMcpToolMetadata Metadata;
	if (!ToolRegistry.TryGetMetadata(ToolName, Metadata))
	{
		// Defensive — registry guarantees metadata is present whenever a handler
		// is. Treat as read-only + gate Off so a corrupt entry still dispatches
		// (the operator can fix the registration separately).
		Metadata = FUnrealOpenMcpToolMetadata::ReadOnly();
	}

	// Gate precedence (docs/api/bridge-http.md#gate-policy):
	//   valid request `gate`  →  tool default.
	// Project default lands between these in a later phase (the project
	// settings surface is not wired yet). Unity adds (2) BridgeGateDefaultPolicy;
	// the Unreal port adds it when the settings tab lands.
	const FString GateToken = ExtractGateToken(Body);
	const EUnrealOpenMcpGateMode EffectiveGate = !GateToken.IsEmpty()
		? FUnrealOpenMcpGatePolicy::ParseMode(GateToken)
		: Metadata.DefaultGate;

	// P3.7 — apply_fix dispatch path special-cases:
	//   1. dry_run (the default) is a no-op mutation — the gate would run a
	//      full checkpoint+validate around a Describe() that changes nothing.
	//      Short-circuit to the inner handler directly so dry-run previews
	//      stay cheap. Unity parity (BridgeHttpServer.cs:773).
	//   2. non-dry-run apply runs through FUnrealOpenMcpApplyFixGateRunner
	//      (rollback snapshot around the gate) — see GameThreadBody.
	//   3. paths_hint auto-derived from issue_id when the caller omits one
	//      (the issue's asset path IS the mutation scope). Mirrors Unity's
	//      BridgeRequestBody.PathsFromIssueId.
	const bool bIsApplyFixTool = ToolName == TEXT("unreal_open_mcp_apply_fix");
	const bool bApplyFixDryRun = bIsApplyFixTool && ExtractBoolField(Body, TEXT("dry_run"), true);

	// paths_hint extraction. Read-only tools skip this (no gate path). Mutating
	// tools MUST supply a non-empty hint — there is no whole-project fallback
	// (Unity parity; pinned by AGENTS.md §Gate policy). The hint may be empty
	// only when gate:"off" is explicitly requested.
	TArray<FString> PathsHint;
	if (Metadata.bIsMutating && EffectiveGate != EUnrealOpenMcpGateMode::Off && !bApplyFixDryRun)
	{
		PathsHint = ExtractPathsHint(Body);
		if (PathsHint.Num() == 0 && bIsApplyFixTool)
		{
			// Auto-derive from issue_id: the issue's asset path is the
			// mutation scope. Parse the canonical key and extract the asset
			// path component (Unity parity).
			const FString IssueId = ExtractStringField(Body, TEXT("issue_id"));
			FString ParsedRuleId;
			EVerifySeverity ParsedSeverity = EVerifySeverity::Warning;
			FString ParsedAssetPath;
			FString ParsedIssueCode;
			if (!IssueId.IsEmpty() && FIssueKey::TryParse(IssueId, ParsedRuleId, ParsedSeverity, ParsedAssetPath, ParsedIssueCode))
			{
				PathsHint.Add(ParsedAssetPath);
			}
		}
		if (PathsHint.Num() == 0)
		{
			// Fail fast BEFORE any mutation runs. The structured error names the
			// tool and the effective gate mode so an agent can self-correct.
			SendJson(Client, 200, FUnrealOpenMcpBridgeEnvelope::BuildPathsHintRequired(ToolName, EffectiveGate));
			return;
		}
	}

	// The fair request queue (RequestQueue) is available on the listener thread
	// for X-Agent-Id-keyed fairness accounting; in the single-stream listener
	// every dispatch is serialized so the queue is exercised by its own spec
	// and wired here for the concurrent-dispatch path a later phase adds.
	RequestQueue.Submit(AgentId, ToolName, []() -> FUnrealOpenMcpToolDispatchResult
	{
		return FUnrealOpenMcpToolDispatchResult::Ok();
	});

	// Marshal the handler (and, for mutating tools, the gate wrapper) onto the
	// game thread. The dispatched body captures ONLY by value — never `this` —
	// so it is safe even if the server is torn down between EnqueueAsync and
	// the game thread draining the body.
	//
	// The body returns BOTH the tool dispatch result AND the gate dispatch
	// result so the listener thread can build the right envelope without
	// re-running the gate. Read-only tools synthesize a Skipped gate result so
	// the envelope builder branches uniformly.
	auto GameThreadBody = [
			Handler = MoveTemp(Handler),
			Body = Body,
			Metadata,
			EffectiveGate,
			PathsHint = MoveTemp(PathsHint),
			bIsApplyFixTool,
			bApplyFixDryRun
		]() -> FUnrealOpenMcpDispatchOutcome
	{
		// Ensure the verify rule registry is populated before any gate pass —
		// the gate flow runs checkpoint + validate over every registered rule.
		// Idempotent (the verify module's StartupModule calls this too).
		FVerifyRunner::EnsureDefaultsRegistered();
		// P3.7 — apply_fix dry-run is a no-op mutation: dispatch directly
		// (the inner handler runs Describe and short-circuits). No gate, no
		// rollback snapshot. The synthetic Skipped outcome keeps the envelope
		// builder uniform.
		if (bIsApplyFixTool && bApplyFixDryRun)
		{
			FUnrealOpenMcpDispatchOutcome Out;
			Out.Tool = FUnrealOpenMcpApplyFixTool::Execute(Body);
			Out.Gate.Mutation = Out.Tool;
			Out.Gate.bGateRan = false;
			Out.Gate.Outcome = Out.Tool.bOk ? EUnrealOpenMcpGateOutcome::Skipped : EUnrealOpenMcpGateOutcome::Failed;
			Out.Gate.bGateFailed = !Out.Tool.bOk;
			return Out;
		}

		// P3.7 — non-dry-run apply_fix runs through the gate runner so a
		// FixRollback snapshot is active (the inner handler refuses a non-
		// dry-run apply without one). The runner reuses FUnrealOpenMcpGatePolicy
		// internally and adds the rollback step.
		if (bIsApplyFixTool)
		{
			const FUnrealOpenMcpApplyFixGateRunnerResult RunnerResult =
				FUnrealOpenMcpApplyFixGateRunner::Execute(Body, EffectiveGate, PathsHint);
			FUnrealOpenMcpDispatchOutcome Out;
			Out.Tool = RunnerResult.Gate.Mutation;
			Out.Gate = RunnerResult.Gate;
			Out.bRolledBack = RunnerResult.bRolledBack;
			Out.bRollbackDisabled = RunnerResult.bRollbackDisabled;
			Out.RollbackReason = RunnerResult.RollbackReason;
			Out.RestoredPaths = RunnerResult.RestoredPaths;
			return Out;
		}

		if (!Metadata.bIsMutating)
		{
			// Read-only: dispatch directly, no gate. The synthetic gate result
			// carries the handler outcome (Skipped on success, Failed on
			// mutation error) so the envelope builder branches uniformly.
			FUnrealOpenMcpDispatchOutcome Out;
			Out.Tool = Handler(Body);
			Out.Gate.Mutation = Out.Tool;
			Out.Gate.bGateRan = false;
			Out.Gate.Outcome = Out.Tool.bOk ? EUnrealOpenMcpGateOutcome::Skipped : EUnrealOpenMcpGateOutcome::Failed;
			Out.Gate.bGateFailed = !Out.Tool.bOk;
			return Out;
		}

		// Mutating: route through the gate. The mutation callback the gate
		// invokes IS the tool handler — the gate owns the checkpoint + validate
		// sandwich around it.
		FUnrealOpenMcpDispatchOutcome MutateOut;
		MutateOut.Gate = FUnrealOpenMcpGatePolicy::Execute(EffectiveGate, PathsHint, [&Handler, &Body]() -> FUnrealOpenMcpToolDispatchResult
		{
			return Handler(Body);
		});
		MutateOut.Tool = MutateOut.Gate.Mutation;
		return MutateOut;
	};

	TFuture<TUnrealOpenMcpDispatchResult<FUnrealOpenMcpDispatchOutcome>> Future =
		Dispatcher.EnqueueAsync<FUnrealOpenMcpDispatchOutcome>(MoveTemp(GameThreadBody), TimeoutMs);

	const TUnrealOpenMcpDispatchResult<FUnrealOpenMcpDispatchOutcome> Result = Future.Get();

	// Map the dispatcher outcome to the canonical envelope. A Success means the
	// body ran (the handler + gate returned their own outcomes); every other
	// outcome is a dispatch-level failure mapped to a distinct error code.
	if (Result.Result == EUnrealOpenMcpDispatchResult::Success)
	{
		const FUnrealOpenMcpDispatchOutcome Outcome = Result.Value.Get(FUnrealOpenMcpDispatchOutcome{});
		const FUnrealOpenMcpToolDispatchResult& ToolResult = Outcome.Tool;

		if (ToolResult.bOk)
		{
			// Success. Mutating tools emit the widened envelope (with gate
			// summary); read-only tools emit the P2.1 shape verbatim so the
			// existing parsers keep working. The gate block is added only when
			// the gate ran (or when the outcome is non-passing, so a Warned
			// mutation surfaces its warnings even though it committed).
			//
			// P3.7 — apply_fix dispatches that rolled back (or committed with
			// gate:"off") emit a `rollback` block alongside the gate summary so
			// an agent sees the rollback decision in a structured field.
			const bool bEmitGateBlock = Metadata.bIsMutating && (Outcome.Gate.bGateRan || Outcome.Gate.bGateFailed);
			const bool bEmitRollbackBlock = Outcome.bRolledBack || Outcome.bRollbackDisabled;
			if (bEmitGateBlock && bEmitRollbackBlock)
			{
				FUnrealOpenMcpBridgeEnvelope::FApplyFixRollbackFields Rollback;
				Rollback.bRolledBack = Outcome.bRolledBack;
				Rollback.bRollbackDisabled = Outcome.bRollbackDisabled;
				Rollback.RollbackReason = Outcome.RollbackReason;
				Rollback.RestoredPaths = Outcome.RestoredPaths;
				SendJson(Client, 200, FUnrealOpenMcpBridgeEnvelope::BuildSuccessWithGateAndRollback(ToolResult.Output, Outcome.Gate, Rollback));
			}
			else if (bEmitGateBlock)
			{
				SendJson(Client, 200, FUnrealOpenMcpBridgeEnvelope::BuildSuccessWithGate(ToolResult.Output, Outcome.Gate));
			}
			else
			{
				SendJson(Client, 200, FUnrealOpenMcpBridgeEnvelope::BuildSuccess(ToolResult.Output));
			}
		}
		else
		{
			// Tool ran and returned a structured failure. Mutating tools emit
			// the gate summary alongside the tool error so an agent sees both.
			if (Metadata.bIsMutating && (Outcome.Gate.bGateRan || Outcome.Gate.bGateFailed))
			{
				SendJson(Client, 200, FUnrealOpenMcpBridgeEnvelope::BuildErrorWithGate(ToolResult.Code, ToolResult.Message, Outcome.Gate));
			}
			else
			{
				SendJson(Client, 200, FUnrealOpenMcpBridgeEnvelope::BuildError(ToolResult.Code, ToolResult.Message));
			}
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
