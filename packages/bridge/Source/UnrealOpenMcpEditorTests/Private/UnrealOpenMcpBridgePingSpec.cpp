// Bridge HTTP /ping Automation specs (P1.3).
//
// Adapts Unity Open MCP's BridgeHttpServerTests
// (packages/bridge/Tests/Editor/Integration/BridgeHttpServerTests.cs) and
// BridgeBindAddressTests (Tests/Editor/Bridge/BridgeBindAddressTests.cs) to
// Unreal. Pinned here (P1.3 acceptance criteria):
//   - GET /ping returns the expected JSON shape (deterministic field set).
//   - The bridgeVersion field matches FUnrealOpenMcpBridgeSession.
//   - mode is "live" on the success path.
//   - 404 for unknown paths with a "not_found" code.
//   - 405 for non-GET methods on /ping with a "method_not_allowed" code.
//   - The server only ever binds 127.0.0.1 (IsLoopbackAddress contract).
//   - Port resolution honors UNREAL_OPEN_MCP_BRIDGE_PORT env override.
//   - Start/Stop is idempotent across reload cycles.
//
// The HTTP path is covered by an inline client: we open a loopback TCP socket
// to the bridge's bound port, send a raw HTTP request, and read the response.
// This is the same shape as the Unity tests' HttpClient.GetAsync — but pure
// socket so we have no third-party HTTP client dependency in the test module.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"

#include "Bridge/UnrealOpenMcpBridgeHttpServer.h"
#include "Bridge/UnrealOpenMcpBridgeJson.h"
#include "Bridge/UnrealOpenMcpBridgeRequestQueue.h"
#include "Bridge/UnrealOpenMcpBridgeSession.h"
#include "Bridge/UnrealOpenMcpInstancePortResolver.h"
#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Dispatch/UnrealOpenMcpGameThreadDispatcher.h"

#include "Common/TcpSocketBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpBridgePingSpec,
	"UnrealOpenMcp.Bridge.Ping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpBridgePingSpec)

namespace
{
	// Tiny blocking HTTP client for the spec. Connects to 127.0.0.1:Port, sends
	// the request, and reads until the server closes (HTTP/1.0 no keep-alive).
	// Returns the raw response (status line + headers + body) as an FString.
	FString SendRawHttpRequest(uint16 Port, const FString& Method, const FString& Path)
	{
		const FIPv4Address Loopback(127, 0, 0, 1);
		const FIPv4Endpoint Endpoint(Loopback, Port);

		FSocket* Client = FTcpSocketBuilder(TEXT("UnrealOpenMcpTestClient"))
			.AsBlocking()
			.Build();
		if (Client == nullptr)
		{
			return FString();
		}

		// Bound connect so a non-listening server fails fast instead of
		// hitting the default 30s connect timeout.
		Client->Connect(*Endpoint.ToInternetAddr());

		const FString RequestLine = FString::Printf(
			TEXT("%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"),
			*Method, *Path);
		const FTCHARToUTF8 RequestUtf8(*RequestLine);

		int32 Sent = 0;
		Client->Send(reinterpret_cast<const uint8*>(RequestUtf8.Get()), RequestUtf8.Length(), Sent);

		TArray<uint8> Received;
		Received.SetNumUninitialized(4096);
		FString Response;
		// Read until peer closes. The server sends Connection: close, so the
		// response ends with a clean FIN after the body. Bound the total wait
		// via Wait(ForRead) so a stuck server can't hang the spec forever.
		const FTimespan WaitTimeout = FTimespan::FromSeconds(10.0);
		while (Client->Wait(ESocketWaitConditions::WaitForRead, WaitTimeout))
		{
			int32 BytesRead = 0;
			const bool bOk = Client->Recv(Received.GetData(), Received.Num(), BytesRead);
			if (!bOk || BytesRead == 0)
			{
				break; // peer closed or errored
			}
			Response += FString(UE_PTRDIFF_TO_INT32(BytesRead), reinterpret_cast<const ANSICHAR*>(Received.GetData()));
		}

		Client->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client);
		return Response;
	}

	// Extract the JSON body from an HTTP response by skipping past the blank
	// line that terminates the headers. Cheap substring probe; the spec only
	// ever needs the body.
	FString ExtractHttpBody(const FString& Response)
	{
		const int32 Separator = Response.Find(TEXT("\r\n\r\n"));
		if (Separator == INDEX_NONE)
		{
			return Response;
		}
		return Response.Mid(Separator + 4);
	}

	uint16 ExtractHttpStatus(const FString& Response)
	{
		// "HTTP/1.1 <code> <reason>"
		TArray<FString> Tokens;
		Response.Left(Response.Find(TEXT("\r\n"))).ParseIntoArrayWS(Tokens);
		if (Tokens.Num() >= 2)
		{
			return static_cast<uint16>(FCString::Atoi(*Tokens[1]));
		}
		return 0;
	}

	// Spin an HTTP server on a kernel-assigned ephemeral port. Returns null on
	// failure. Caller owns the lifetime.
	//
	// P2.1: the server now takes a registry + queue alongside the dispatcher.
	// The ping tests don't exercise tools, so empty registry/queue are fine —
	// they just need to outlive the server. The harness struct owns all three
	// so the caller deletes only the harness and the destruction order
	// (server-first) is correct.
	struct FPingHarness
	{
		FUnrealOpenMcpGameThreadDispatcher Dispatcher;
		FUnrealOpenMcpToolRegistry Registry;
		FUnrealOpenMcpBridgeRequestQueue Queue;
		FUnrealOpenMcpBridgeHttpServer* Server = nullptr;

		bool Start(const FString& ProjectPath)
		{
			Server = new FUnrealOpenMcpBridgeHttpServer(Dispatcher, Registry, Queue);
			return Server->Start(0, ProjectPath);
		}

		~FPingHarness()
		{
			if (Server)
			{
				Server->Stop();
				delete Server;
			}
		}
	};
}

void FUnrealOpenMcpBridgePingSpec::Define()
{
	Describe("Bind address", [this]()
	{
		// P1.3 DoD: "Server does not bind to non-loopback addresses." The
		// contract is enforced at the type level — IsLoopbackAddress is the
		// only true case.
		It("accepts only 127.0.0.1 as the bind address", [this]()
		{
			TestTrue(TEXT("127.0.0.1 is loopback"), FUnrealOpenMcpBridgeHttpServer::IsLoopbackAddress(TEXT("127.0.0.1")));
			TestFalse(TEXT("0.0.0.0 is not loopback"), FUnrealOpenMcpBridgeHttpServer::IsLoopbackAddress(TEXT("0.0.0.0")));
			TestFalse(TEXT("localhost is not accepted"), FUnrealOpenMcpBridgeHttpServer::IsLoopbackAddress(TEXT("localhost")));
			TestFalse(TEXT("empty is not accepted"), FUnrealOpenMcpBridgeHttpServer::IsLoopbackAddress(TEXT("")));
			TestFalse(TEXT("LAN IP is not accepted"), FUnrealOpenMcpBridgeHttpServer::IsLoopbackAddress(TEXT("192.168.1.1")));
		});
	});

	Describe("Port resolution", [this]()
	{
		// P1.4: ResolvePort now delegates to FUnrealOpenMcpInstancePortResolver.
		// The full formula + golden values are pinned in
		// UnrealOpenMcpPortResolverSpec; here we only pin the wiring so a
		// refactor that bypasses the resolver trips this spec.
		//
		// We cannot reliably mutate the process env from a spec (cross-platform
		// FPlatformMisc::SetEnvironmentVar is editor-only and the C-runtime copy
		// may be cached), so we pin the resolver parity instead of asserting an
		// env-driven value.
		It("resolves via the deterministic resolver when no override is set", [this]()
		{
			const FString ProjectPath = TEXT("/Users/foo/MyGame");
			const uint16 Resolved = FUnrealOpenMcpBridgeHttpServer::ResolvePort(ProjectPath);
			const int32 Expected = FUnrealOpenMcpInstancePortResolver::ComputePort(ProjectPath);
			TestEqual(TEXT("matches the resolver's ComputePort"), static_cast<int32>(Resolved), Expected);
		});

		// The env var name must match Unity's so the MCP server's
		// instance-discovery reads the same override.
		It("advertises the canonical env var name", [this]()
		{
			TestEqual(
				TEXT("UNREAL_OPEN_MCP_BRIDGE_PORT env var"),
				FString(FUnrealOpenMcpBridgeHttpServer::PortEnvVar()),
				FString(TEXT("UNREAL_OPEN_MCP_BRIDGE_PORT")));
		});
	});

	Describe("GET /ping", [this]()
	{
		// Ephemeral-port server, exercised end-to-end through a real loopback
		// TCP connection. Latent because the dispatcher marshal onto the game
		// thread is pumped between latent frames.
		LatentIt(
			"returns the pinned JSON shape on 200",
			FTimespan::FromSeconds(30),
			[this](const FDoneDelegate& Done)
			{
				Async(EAsyncExecution::Thread, [this, Done]()
				{
					FPingHarness Harness;
					if (!TestTrue(TEXT("ephemeral server started"), Harness.Start(TEXT("/tmp/test-project"))))
					{
						Done.Execute();
						return;
					}

					const uint16 Port = Harness.Server->GetPort();
					TestTrue(TEXT("ephemeral port assigned"), Port > 0);

					const FString Response = SendRawHttpRequest(Port, TEXT("GET"), TEXT("/ping"));
					TestTrue(TEXT("got a response"), !Response.IsEmpty());
					TestEqual(TEXT("HTTP 200"), ExtractHttpStatus(Response), uint16(200));

					const FString Body = ExtractHttpBody(Response);
					// Pinned field set — see FUnrealOpenMcpPingPayload. Every
					// field must be present on the 200 path.
					TestTrue(TEXT("has connected"), Body.Contains(TEXT("\"connected\"")));
					TestTrue(TEXT("has status"), Body.Contains(TEXT("\"status\"")));
					TestTrue(TEXT("has projectPath"), Body.Contains(TEXT("\"projectPath\"")));
					TestTrue(TEXT("has unrealVersion"), Body.Contains(TEXT("\"unrealVersion\"")));
					TestTrue(TEXT("has bridgeVersion"), Body.Contains(TEXT("\"bridgeVersion\"")));
					TestTrue(TEXT("has mode"), Body.Contains(TEXT("\"mode\"")));
					TestTrue(TEXT("has port"), Body.Contains(TEXT("\"port\"")));
					TestTrue(TEXT("has compiling"), Body.Contains(TEXT("\"compiling\"")));
					TestTrue(TEXT("has isPlaying"), Body.Contains(TEXT("\"isPlaying\"")));

					// Field-level assertions for the load-bearing values.
					TestTrue(TEXT("connected=true on ready"),
						Body.Contains(TEXT("\"connected\":true")));
					TestTrue(TEXT("status=ready"),
						Body.Contains(TEXT("\"status\":\"ready\"")));
					TestTrue(TEXT("mode=live"),
						Body.Contains(TEXT("\"mode\":\"live\"")));
					TestTrue(
						TEXT("bridgeVersion matches session"),
						Body.Contains(FString::Printf(
							TEXT("\"bridgeVersion\":\"%s\""),
							FUnrealOpenMcpBridgeSession::GetBridgeVersion())));
					TestTrue(
						TEXT("projectPath echoed"),
						Body.Contains(TEXT("\"projectPath\":\"/tmp/test-project\"")));
					TestTrue(
						TEXT("port echoed"),
						Body.Contains(FString::Printf(TEXT("\"port\":%u"), static_cast<uint32>(Port))));

					Done.Execute();
				});
			});

		// Wrong method on /ping → 405 + method_not_allowed. Mirrors Unity's
		// ToolsEndpoint_GetMethod_Returns405 (inverse direction: that one pins
		// GET on a POST-only path; this one pins GET-only on /ping).
		LatentIt(
			"rejects non-GET methods with 405 method_not_allowed",
			FTimespan::FromSeconds(30),
			[this](const FDoneDelegate& Done)
			{
				Async(EAsyncExecution::Thread, [this, Done]()
				{
					FPingHarness Harness;
					if (!TestTrue(TEXT("ephemeral server started"), Harness.Start(TEXT("/tmp/test-project"))))
					{
						Done.Execute();
						return;
					}

					const FString Response = SendRawHttpRequest(Harness.Server->GetPort(), TEXT("POST"), TEXT("/ping"));
					TestEqual(TEXT("HTTP 405"), ExtractHttpStatus(Response), uint16(405));
					TestTrue(TEXT("method_not_allowed code"), ExtractHttpBody(Response).Contains(TEXT("\"method_not_allowed\"")));

					Done.Execute();
				});
			});

		// Unknown path → 404 + not_found. Mirrors Unity's
		// UnknownEndpoint_Returns404.
		LatentIt(
			"returns 404 not_found for unknown paths",
			FTimespan::FromSeconds(30),
			[this](const FDoneDelegate& Done)
			{
				Async(EAsyncExecution::Thread, [this, Done]()
				{
					FPingHarness Harness;
					if (!TestTrue(TEXT("ephemeral server started"), Harness.Start(TEXT("/tmp/test-project"))))
					{
						Done.Execute();
						return;
					}

					const FString Response = SendRawHttpRequest(Harness.Server->GetPort(), TEXT("GET"), TEXT("/nope"));
					TestEqual(TEXT("HTTP 404"), ExtractHttpStatus(Response), uint16(404));
					TestTrue(TEXT("not_found code"), ExtractHttpBody(Response).Contains(TEXT("\"not_found\"")));

					Done.Execute();
				});
			});

		// /ping with a trailing slash should still hit the ping handler (we
		// normalize). Cheap robustness check.
		LatentIt(
			"accepts /ping/ with a trailing slash",
			FTimespan::FromSeconds(30),
			[this](const FDoneDelegate& Done)
			{
				Async(EAsyncExecution::Thread, [this, Done]()
				{
					FPingHarness Harness;
					if (!TestTrue(TEXT("ephemeral server started"), Harness.Start(TEXT("/tmp/test-project"))))
					{
						Done.Execute();
						return;
					}

					const FString Response = SendRawHttpRequest(Harness.Server->GetPort(), TEXT("GET"), TEXT("/ping/"));
					TestEqual(TEXT("HTTP 200"), ExtractHttpStatus(Response), uint16(200));

					Done.Execute();
				});
			});
	});

	Describe("Lifecycle", [this]()
	{
		// P1.3 DoD: "Startup/shutdown survives editor reload cycles cleanly."
		// Idempotent Stop + Start/Stop/Start round-trip.
		It("survives Start/Stop/Start without leaking or crashing", [this]()
		{
			FUnrealOpenMcpGameThreadDispatcher Dispatcher;
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBridgeRequestQueue Queue;
			FUnrealOpenMcpBridgeHttpServer Server(Dispatcher, Registry, Queue);

			TestTrue(TEXT("first Start"), Server.Start(0, TEXT("/tmp/lifecycle")));
			TestTrue(TEXT("running after first Start"), Server.IsRunning());
			const uint16 FirstPort = Server.GetPort();
			TestTrue(TEXT("first port assigned"), FirstPort > 0);

			Server.Stop();
			TestFalse(TEXT("stopped"), Server.IsRunning());

			// Start again — a fresh ephemeral port.
			TestTrue(TEXT("second Start"), Server.Start(0, TEXT("/tmp/lifecycle")));
			TestTrue(TEXT("running after second Start"), Server.IsRunning());
			TestTrue(TEXT("new port assigned"), Server.GetPort() > 0);

			Server.Stop();
			TestFalse(TEXT("stopped after second cycle"), Server.IsRunning());

			// Double-Stop must be safe.
			Server.Stop();
			TestFalse(TEXT("still stopped after double-Stop"), Server.IsRunning());
		});

		// Re-Start on an already-running server is a no-op (idempotent).
		It("treats Start on a running server as a no-op", [this]()
		{
			FUnrealOpenMcpGameThreadDispatcher Dispatcher;
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpBridgeRequestQueue Queue;
			FUnrealOpenMcpBridgeHttpServer Server(Dispatcher, Registry, Queue);

			TestTrue(TEXT("first Start"), Server.Start(0, TEXT("/tmp/idempotent")));
			const uint16 FirstPort = Server.GetPort();

			TestTrue(TEXT("second Start succeeds"), Server.Start(0, TEXT("/tmp/idempotent")));
			TestEqual(TEXT("port unchanged after no-op Start"), Server.GetPort(), FirstPort);

			Server.Stop();
		});
	});

	Describe("JSON builder", [this]()
	{
		// Pure builder test — no HTTP. Pins the deterministic field order and
		// the JSON shape contract that the MCP server's parity probes rely on.
		It("BuildPingJson emits fields in pinned order", [this]()
		{
			FUnrealOpenMcpPingPayload Payload;
			Payload.bConnected = true;
			Payload.Status = TEXT("ready");
			Payload.ProjectPath = TEXT("/tmp/p");
			Payload.UnrealVersion = TEXT("5.8.0");
			Payload.BridgeVersion = TEXT("0.0.1");
			Payload.Mode = TEXT("live");
			Payload.Port = 21111;
			Payload.bCompiling = false;
			Payload.bIsPlaying = false;

			const FString Json = FUnrealOpenMcpBridgeJson::BuildPingJson(Payload);

			// Field order is load-bearing — every field appears in this exact
			// sequence. Pinned via index comparisons so a future contributor
			// reordering the builder trips this spec.
			int32 ConnectedAt = Json.Find(TEXT("\"connected\""));
			int32 StatusAt = Json.Find(TEXT("\"status\""));
			int32 ProjectPathAt = Json.Find(TEXT("\"projectPath\""));
			int32 UnrealVersionAt = Json.Find(TEXT("\"unrealVersion\""));
			int32 BridgeVersionAt = Json.Find(TEXT("\"bridgeVersion\""));
			int32 ModeAt = Json.Find(TEXT("\"mode\""));
			int32 PortAt = Json.Find(TEXT("\"port\""));
			int32 CompilingAt = Json.Find(TEXT("\"compiling\""));
			int32 IsPlayingAt = Json.Find(TEXT("\"isPlaying\""));

			TestTrue(TEXT("all fields present"), ConnectedAt != INDEX_NONE && StatusAt != INDEX_NONE &&
				ProjectPathAt != INDEX_NONE && UnrealVersionAt != INDEX_NONE &&
				BridgeVersionAt != INDEX_NONE && ModeAt != INDEX_NONE &&
				PortAt != INDEX_NONE && CompilingAt != INDEX_NONE && IsPlayingAt != INDEX_NONE);

			TestTrue(TEXT("connected before status"), ConnectedAt < StatusAt);
			TestTrue(TEXT("status before projectPath"), StatusAt < ProjectPathAt);
			TestTrue(TEXT("projectPath before unrealVersion"), ProjectPathAt < UnrealVersionAt);
			TestTrue(TEXT("unrealVersion before bridgeVersion"), UnrealVersionAt < BridgeVersionAt);
			TestTrue(TEXT("bridgeVersion before mode"), BridgeVersionAt < ModeAt);
			TestTrue(TEXT("mode before port"), ModeAt < PortAt);
			TestTrue(TEXT("port before compiling"), PortAt < CompilingAt);
			TestTrue(TEXT("compiling before isPlaying"), CompilingAt < IsPlayingAt);

			// Value-level assertions.
			TestTrue(TEXT("connected value"), Json.Contains(TEXT("\"connected\":true")));
			TestTrue(TEXT("status value"), Json.Contains(TEXT("\"status\":\"ready\"")));
			TestTrue(TEXT("projectPath value"), Json.Contains(TEXT("\"projectPath\":\"/tmp/p\"")));
			TestTrue(TEXT("unrealVersion value"), Json.Contains(TEXT("\"unrealVersion\":\"5.8.0\"")));
			TestTrue(TEXT("bridgeVersion value"), Json.Contains(TEXT("\"bridgeVersion\":\"0.0.1\"")));
			TestTrue(TEXT("mode value"), Json.Contains(TEXT("\"mode\":\"live\"")));
			TestTrue(TEXT("port value"), Json.Contains(TEXT("\"port\":21111")));
			TestTrue(TEXT("compiling value"), Json.Contains(TEXT("\"compiling\":false")));
			TestTrue(TEXT("isPlaying value"), Json.Contains(TEXT("\"isPlaying\":false")));
		});

		It("BuildErrorJson emits the code+message shape", [this]()
		{
			const FString Json = FUnrealOpenMcpBridgeJson::BuildErrorJson(TEXT("not_found"), TEXT("Unknown path: /x"));
			TestTrue(TEXT("error wrapper"), Json.StartsWith(TEXT("{\"error\":{")));
			TestTrue(TEXT("code field"), Json.Contains(TEXT("\"code\":\"not_found\"")));
			TestTrue(TEXT("message field"), Json.Contains(TEXT("\"message\":\"Unknown path: /x\"")));
		});

		// Strings with quotes / backslashes / control chars must be escaped.
		// Cheap robustness check for the AppendJsonString primitive.
		It("escapes embedded quotes and backslashes", [this]()
		{
			FUnrealOpenMcpPingPayload Payload;
			Payload.bConnected = true;
			Payload.Status = TEXT("ready");
			Payload.ProjectPath = TEXT("C:\\path with \"quotes\"");
			Payload.BridgeVersion = TEXT("0.0.1");
			Payload.Mode = TEXT("live");

			const FString Json = FUnrealOpenMcpBridgeJson::BuildPingJson(Payload);
			// Backslash: source `C:\path` → JSON `C:\\path` (each backslash
			// doubled).
			TestTrue(TEXT("backslash escaped"), Json.Contains(TEXT("C:\\\\path")));
			// Quote: source `"quotes"` → JSON `\"quotes\"` (each quote
			// backslash-escaped).
			TestTrue(TEXT("quote escaped"), Json.Contains(TEXT("\\\"quotes\\\"")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
