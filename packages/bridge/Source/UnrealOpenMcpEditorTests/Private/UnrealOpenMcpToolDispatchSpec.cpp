// Tool dispatch (POST /tools/{name}) Automation specs (P2.1).
//
// Adapts Unity Open MCP's tool-dispatch integration tests
// (packages/bridge/Tests/Editor/Integration/BridgeHttpServerTests.cs — the
// /tools/{name} cases) to Unreal. Pinned here (P2.1 acceptance criteria):
//   - POST /tools/unreal_open_mcp_echo returns the {ok,result} success
//     envelope with the echoed body (HTTP 200).
//   - Unknown tool → HTTP 404 with {error:{code:"tool_not_found",...}}.
//   - GET on a tool endpoint → HTTP 405 method_not_allowed.
//   - Tool that returns a structured failure → HTTP 200 with {ok:false,error}.
//   - The canonical envelope shape is pinned (ok / result / error.code).
//
// Also unit-pins the registry + fair queue contracts:
//   - Registry Register (first-wins), Contains, TryGet, AllNames.
//   - Queue per-agent FIFO + round-robin across agents (X-Agent-Id keyed).
//
// The HTTP path is covered by an inline socket client (same shape as the P1.3
// ping spec) extended to send a POST body + custom headers.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"

#include "Bridge/UnrealOpenMcpBridgeEnvelope.h"
#include "Bridge/UnrealOpenMcpBridgeHttpServer.h"
#include "Bridge/UnrealOpenMcpBridgeRequestQueue.h"
#include "Bridge/UnrealOpenMcpBridgeJson.h"
#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Dispatch/UnrealOpenMcpGameThreadDispatcher.h"

#include "Common/TcpSocketBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpToolDispatchSpec,
	"UnrealOpenMcp.Bridge.ToolDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpToolDispatchSpec)

namespace
{
	/**
	 * Send a raw HTTP request with an optional body + custom headers, then read
	 * the full response (status line + headers + body) until the server closes.
	 * Extended from the P1.3 ping spec's helper to support POST bodies and the
	 * X-Agent-Id header the fair queue keys on.
	 */
	FString SendHttp(
		uint16 Port,
		const FString& Method,
		const FString& Path,
		const FString& Body = FString(),
		const TArray<TPair<FString, FString>>& ExtraHeaders = {})
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

		Client->Connect(*Endpoint.ToInternetAddr());

		const FTCHARToUTF8 BodyUtf8(*Body);
		const int32 BodyLen = BodyUtf8.Length();

		FString Request = FString::Printf(
			TEXT("%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"),
			*Method, *Path);
		if (BodyLen > 0)
		{
			Request += FString::Printf(TEXT("Content-Length: %d\r\n"), BodyLen);
			Request += TEXT("Content-Type: application/json; charset=utf-8\r\n");
		}
		for (const auto& Header : ExtraHeaders)
		{
			Request += FString::Printf(TEXT("%s: %s\r\n"), *Header.Key, *Header.Value);
		}
		Request += TEXT("\r\n");

		const FTCHARToUTF8 RequestUtf8(*Request);
		int32 Sent = 0;
		Client->Send(reinterpret_cast<const uint8*>(RequestUtf8.Get()), RequestUtf8.Length(), Sent);
		if (BodyLen > 0)
		{
			Client->Send(reinterpret_cast<const uint8*>(BodyUtf8.Get()), BodyLen, Sent);
		}

		TArray<uint8> Received;
		Received.SetNumUninitialized(4096);
		FString Response;
		const FTimespan WaitTimeout = FTimespan::FromSeconds(15.0);
		while (Client->Wait(ESocketWaitConditions::WaitForRead, WaitTimeout))
		{
			int32 BytesRead = 0;
			const bool bOk = Client->Recv(Received.GetData(), Received.Num(), BytesRead);
			if (!bOk || BytesRead == 0)
			{
				break;
			}
			Response += FString(UE_PTRDIFF_TO_INT32(BytesRead), reinterpret_cast<const ANSICHAR*>(Received.GetData()));
		}

		Client->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client);
		return Response;
	}

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
		TArray<FString> Tokens;
		Response.Left(Response.Find(TEXT("\r\n"))).ParseIntoArrayWS(Tokens);
		if (Tokens.Num() >= 2)
		{
			return static_cast<uint16>(FCString::Atoi(*Tokens[1]));
		}
		return 0;
	}

	/** Start an ephemeral server wired with a fresh registry + queue. Caller
	 *  owns the registry, queue, dispatcher, and server — all must outlive the
	 *  server and be torn down server-first. */
	struct FDispatchHarness
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

		void Stop()
		{
			if (Server)
			{
				Server->Stop();
				delete Server;
				Server = nullptr;
			}
		}
	};
}

void FUnrealOpenMcpToolDispatchSpec::Define()
{
	Describe("POST /tools/{name} dispatch", [this]()
	{
		// Echo round-trip — the P2.1 smoke stub. Pins the full path:
		// POST → ReadRequest → registry lookup → game-thread handler →
		// {ok,result} envelope (HTTP 200).
		LatentIt(
			"echo returns the {ok,result} success envelope on 200",
			FTimespan::FromSeconds(30),
			[this](const FDoneDelegate& Done)
			{
				Async(EAsyncExecution::Thread, [this, Done]()
				{
					FDispatchHarness Harness;
					Harness.Registry.Register(
						TEXT("unreal_open_mcp_echo"),
						[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
						{
							FString EchoValue = Body.TrimStartAndEnd().IsEmpty() ? FString(TEXT("null")) : Body;
							return FUnrealOpenMcpToolDispatchResult::Ok(FString::Printf(TEXT("{\"echo\":%s}"), *EchoValue));
						});
					if (!TestTrue(TEXT("server started"), Harness.Start(TEXT("/tmp/test"))))
					{
						Done.Execute();
						return;
					}

					const FString Response = SendHttp(
						Harness.Server->GetPort(),
						TEXT("POST"),
						TEXT("/tools/unreal_open_mcp_echo"),
						TEXT("{\"hello\":\"world\"}"));
					TestEqual(TEXT("HTTP 200"), ExtractHttpStatus(Response), uint16(200));

					const FString Body = ExtractHttpBody(Response);
					TestTrue(TEXT("ok:true"), Body.Contains(TEXT("\"ok\":true")));
					TestTrue(TEXT("result present"), Body.Contains(TEXT("\"result\":")));
					TestTrue(TEXT("echo echoed"), Body.Contains(TEXT("\"echo\":{\"hello\":\"world\"}")));
					TestFalse(TEXT("no top-level error"), Body.Contains(TEXT("\"error\"")));

					Harness.Stop();
					Done.Execute();
				});
			});

		// Unknown tool → HTTP 404 with tool_not_found. Pins the routing
		// failure path (registry miss → BuildToolNotFound → 404).
		LatentIt(
			"returns 404 tool_not_found for an unknown tool",
			FTimespan::FromSeconds(30),
			[this](const FDoneDelegate& Done)
			{
				Async(EAsyncExecution::Thread, [this, Done]()
				{
					FDispatchHarness Harness;
					if (!TestTrue(TEXT("server started"), Harness.Start(TEXT("/tmp/test"))))
					{
						Done.Execute();
						return;
					}

					const FString Response = SendHttp(
						Harness.Server->GetPort(),
						TEXT("POST"),
						TEXT("/tools/unreal_open_mcp_does_not_exist"),
						TEXT("{}"));
					TestEqual(TEXT("HTTP 404"), ExtractHttpStatus(Response), uint16(404));

					const FString Body = ExtractHttpBody(Response);
					TestTrue(TEXT("tool_not_found code"), Body.Contains(TEXT("\"code\":\"tool_not_found\"")));
					TestTrue(TEXT("names the tool"), Body.Contains(TEXT("unreal_open_mcp_does_not_exist")));

					Harness.Stop();
					Done.Execute();
				});
			});

		// GET on a tool endpoint → 405 method_not_allowed. Mirrors the /ping
		// method guard (POST required for tools, GET required for /ping).
		LatentIt(
			"rejects GET on a tool endpoint with 405 method_not_allowed",
			FTimespan::FromSeconds(30),
			[this](const FDoneDelegate& Done)
			{
				Async(EAsyncExecution::Thread, [this, Done]()
				{
					FDispatchHarness Harness;
					Harness.Registry.Register(
						TEXT("unreal_open_mcp_echo"),
						[](const FString&) -> FUnrealOpenMcpToolDispatchResult
						{
							return FUnrealOpenMcpToolDispatchResult::Ok(TEXT("{}"));
						});
					if (!TestTrue(TEXT("server started"), Harness.Start(TEXT("/tmp/test"))))
					{
						Done.Execute();
						return;
					}

					const FString Response = SendHttp(
						Harness.Server->GetPort(),
						TEXT("GET"),
						TEXT("/tools/unreal_open_mcp_echo"));
					TestEqual(TEXT("HTTP 405"), ExtractHttpStatus(Response), uint16(405));
					TestTrue(TEXT("method_not_allowed code"), ExtractHttpBody(Response).Contains(TEXT("\"method_not_allowed\"")));

					Harness.Stop();
					Done.Execute();
				});
			});

		// A tool that returns a structured failure → HTTP 200 {ok:false}.
		// Structured tool outcomes are never transport failures.
		LatentIt(
			"surfaces a tool-level failure as {ok:false} on HTTP 200",
			FTimespan::FromSeconds(30),
			[this](const FDoneDelegate& Done)
			{
				Async(EAsyncExecution::Thread, [this, Done]()
				{
					FDispatchHarness Harness;
					Harness.Registry.Register(
						TEXT("unreal_open_mcp_fail_test"),
						[](const FString&) -> FUnrealOpenMcpToolDispatchResult
						{
							return FUnrealOpenMcpToolDispatchResult::Fail(
								TEXT("invalid_request"),
								TEXT("Arguments were not valid."));
						});
					if (!TestTrue(TEXT("server started"), Harness.Start(TEXT("/tmp/test"))))
					{
						Done.Execute();
						return;
					}

					const FString Response = SendHttp(
						Harness.Server->GetPort(),
						TEXT("POST"),
						TEXT("/tools/unreal_open_mcp_fail_test"),
						TEXT("{}"));
					TestEqual(TEXT("HTTP 200 (structured failure)"), ExtractHttpStatus(Response), uint16(200));

					const FString Body = ExtractHttpBody(Response);
					TestTrue(TEXT("ok:false"), Body.Contains(TEXT("\"ok\":false")));
					TestTrue(TEXT("error.code"), Body.Contains(TEXT("\"code\":\"invalid_request\"")));
					TestTrue(TEXT("error.message"), Body.Contains(TEXT("Arguments were not valid")));

					Harness.Stop();
					Done.Execute();
				});
			});

		// X-Agent-Id header flows to the queue — cheap parity that the header
		// is parsed (the queue records the agent id). Verified indirectly: the
		// dispatch succeeds with the header present.
		LatentIt(
			"accepts the X-Agent-Id header without breaking dispatch",
			FTimespan::FromSeconds(30),
			[this](const FDoneDelegate& Done)
			{
				Async(EAsyncExecution::Thread, [this, Done]()
				{
					FDispatchHarness Harness;
					Harness.Registry.Register(
						TEXT("unreal_open_mcp_echo"),
						[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
						{
							return FUnrealOpenMcpToolDispatchResult::Ok(Body.IsEmpty() ? TEXT("null") : Body);
						});
					if (!TestTrue(TEXT("server started"), Harness.Start(TEXT("/tmp/test"))))
					{
						Done.Execute();
						return;
					}

					TArray<TPair<FString, FString>> Headers;
					Headers.Add(TPair<FString, FString>(TEXT("X-Agent-Id"), TEXT("agent-42")));
					const FString Response = SendHttp(
						Harness.Server->GetPort(),
						TEXT("POST"),
						TEXT("/tools/unreal_open_mcp_echo"),
						TEXT("123"),
						Headers);
					TestEqual(TEXT("HTTP 200"), ExtractHttpStatus(Response), uint16(200));
					TestTrue(TEXT("ok:true"), ExtractHttpBody(Response).Contains(TEXT("\"ok\":true")));

					Harness.Stop();
					Done.Execute();
				});
			});
	});

	Describe("Envelope builders", [this]()
	{
		// Pure builder tests — no HTTP. Pin the {ok,result,error} shape the MCP
		// side parses.
		It("BuildSuccess emits {ok:true,result:<value>}", [this]()
		{
			const FString Envelope = FUnrealOpenMcpBridgeEnvelope::BuildSuccess(TEXT("{\"echo\":1}"));
			TestTrue(TEXT("ok true"), Envelope.Contains(TEXT("\"ok\":true")));
			TestTrue(TEXT("result value spliced"), Envelope.Contains(TEXT("\"result\":{\"echo\":1}")));
		});

		It("BuildSuccess emits null for an empty result", [this]()
		{
			const FString Envelope = FUnrealOpenMcpBridgeEnvelope::BuildSuccess(FString());
			TestTrue(TEXT("result null"), Envelope.Contains(TEXT("\"result\":null")));
		});

		It("BuildError emits {ok:false,error:{code,message}}", [this]()
		{
			const FString Envelope = FUnrealOpenMcpBridgeEnvelope::BuildError(TEXT("timeout"), TEXT("ran out of time"));
			TestTrue(TEXT("ok false"), Envelope.Contains(TEXT("\"ok\":false")));
			TestTrue(TEXT("error code"), Envelope.Contains(TEXT("\"code\":\"timeout\"")));
			TestTrue(TEXT("error message"), Envelope.Contains(TEXT("\"message\":\"ran out of time\"")));
		});

		It("BuildToolNotFound emits the bare error shape", [this]()
		{
			const FString Body = FUnrealOpenMcpBridgeEnvelope::BuildToolNotFound(TEXT("foo"));
			TestTrue(TEXT("no ok wrapper"), !Body.Contains(TEXT("\"ok\"")));
			TestTrue(TEXT("tool_not_found code"), Body.Contains(TEXT("\"code\":\"tool_not_found\"")));
			TestTrue(TEXT("names the tool"), Body.Contains(TEXT("Unknown tool: foo")));
		});

		// Escape robustness — error messages with quotes must not break JSON.
		It("BuildError escapes quotes in the message", [this]()
		{
			const FString Envelope = FUnrealOpenMcpBridgeEnvelope::BuildError(TEXT("bad"), TEXT("path \"x\" is invalid"));
			TestTrue(TEXT("quote escaped"), Envelope.Contains(TEXT("\\\"x\\\"")));
		});
	});

	Describe("Tool registry", [this]()
	{
		It("registers, contains, and resolves a handler", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			const bool bRegistered = Registry.Register(
				TEXT("unreal_open_mcp_one"),
				[](const FString&) -> FUnrealOpenMcpToolDispatchResult
				{
					return FUnrealOpenMcpToolDispatchResult::Ok(TEXT("1"));
				});
			TestTrue(TEXT("first Register succeeds"), bRegistered);
			TestTrue(TEXT("Contains"), Registry.Contains(TEXT("unreal_open_mcp_one")));
			TestEqual(TEXT("Num"), Registry.Num(), 1);

			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("TryGet"), Registry.TryGet(TEXT("unreal_open_mcp_one"), Handler));
			if (TestNotNull(TEXT("handler callable"), &Handler))
			{
				const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT(""));
				TestTrue(TEXT("Ok"), Result.bOk);
				TestEqual(TEXT("output"), Result.Output, FString(TEXT("1")));
			}
		});

		It("first registration wins on a duplicate name", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			Registry.Register(
				TEXT("unreal_open_mcp_dup"),
				[](const FString&) -> FUnrealOpenMcpToolDispatchResult
				{
					return FUnrealOpenMcpToolDispatchResult::Ok(TEXT("first"));
				});
			const bool bSecond = Registry.Register(
				TEXT("unreal_open_mcp_dup"),
				[](const FString&) -> FUnrealOpenMcpToolDispatchResult
				{
					return FUnrealOpenMcpToolDispatchResult::Ok(TEXT("second"));
				});
			TestFalse(TEXT("second Register rejected"), bSecond);

			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_dup"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT(""));
			TestEqual(TEXT("first handler kept"), Result.Output, FString(TEXT("first")));
		});

		It("TryGet returns false for an unregistered name", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpToolHandler Handler;
			TestFalse(TEXT("miss"), Registry.TryGet(TEXT("unreal_open_mcp_nope"), Handler));
		});

		It("AllNames returns all registered names", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			Registry.Register(TEXT("unreal_open_mcp_a"), [](const FString&) { return FUnrealOpenMcpToolDispatchResult::Ok(); });
			Registry.Register(TEXT("unreal_open_mcp_b"), [](const FString&) { return FUnrealOpenMcpToolDispatchResult::Ok(); });
			const TArray<FString> Names = Registry.AllNames();
			TestEqual(TEXT("two names"), Names.Num(), 2);
			TestTrue(TEXT("has a"), Names.Contains(TEXT("unreal_open_mcp_a")));
			TestTrue(TEXT("has b"), Names.Contains(TEXT("unreal_open_mcp_b")));
		});
	});

	Describe("Request queue", [this]()
	{
		// Per-agent FIFO — a single agent's requests drain in arrival order.
		It("drains one agent's requests in FIFO order", [this]()
		{
			FUnrealOpenMcpBridgeRequestQueue Queue;
			int32 Counter = 0;
			Queue.Enqueue(TEXT("agent-a"), TEXT("t"), [&]() -> FUnrealOpenMcpToolDispatchResult
			{
				Counter = 1;
				return FUnrealOpenMcpToolDispatchResult::Ok();
			});
			Queue.Enqueue(TEXT("agent-a"), TEXT("t"), [&]() -> FUnrealOpenMcpToolDispatchResult
			{
				Counter = 2;
				return FUnrealOpenMcpToolDispatchResult::Ok();
			});
			TestEqual(TEXT("two pending"), Queue.PendingCount(), 2);
			TestEqual(TEXT("one agent"), Queue.AgentCount(), 1);

			FUnrealOpenMcpQueuedRequest R1;
			TestTrue(TEXT("pick 1"), Queue.PickNext(R1));
			R1.Work();
			TestEqual(TEXT("first ran"), Counter, 1);

			FUnrealOpenMcpQueuedRequest R2;
			TestTrue(TEXT("pick 2"), Queue.PickNext(R2));
			R2.Work();
			TestEqual(TEXT("second ran"), Counter, 2);

			FUnrealOpenMcpQueuedRequest R3;
			TestFalse(TEXT("queue drained"), Queue.PickNext(R3));
		});

		// Round-robin across agents — two agents' requests interleave so no
		// agent is starved. The cursor advances past each picked agent.
		It("round-robins across two agents without starvation", [this]()
		{
			FUnrealOpenMcpBridgeRequestQueue Queue;
			// Agent A queues two; agent B queues one. Round-robin order must
			// visit A, then B, then A again (not A, A, B).
			TArray<FString> Order;
			Queue.Enqueue(TEXT("A"), TEXT("t"), [&]() -> FUnrealOpenMcpToolDispatchResult
			{
				Order.Add(TEXT("A1"));
				return FUnrealOpenMcpToolDispatchResult::Ok();
			});
			Queue.Enqueue(TEXT("B"), TEXT("t"), [&]() -> FUnrealOpenMcpToolDispatchResult
			{
				Order.Add(TEXT("B1"));
				return FUnrealOpenMcpToolDispatchResult::Ok();
			});
			Queue.Enqueue(TEXT("A"), TEXT("t"), [&]() -> FUnrealOpenMcpToolDispatchResult
			{
				Order.Add(TEXT("A2"));
				return FUnrealOpenMcpToolDispatchResult::Ok();
			});

			FUnrealOpenMcpQueuedRequest R;
			Queue.PickNext(R); R.Work();
			Queue.PickNext(R); R.Work();
			Queue.PickNext(R); R.Work();

			TestEqual(TEXT("three picked"), Order.Num(), 3);
			// First two must be from different agents (interleaved).
			TestNotEqual(TEXT("first != second agent"), Order[0][0], Order[1][0]);
			// The third returns to the first agent (round-robin wrap).
			TestEqual(TEXT("third == first agent"), Order[2][0], Order[0][0]);
		});

		// Submit runs the work inline and returns its result.
		It("Submit runs the work and returns its result", [this]()
		{
			FUnrealOpenMcpBridgeRequestQueue Queue;
			const FUnrealOpenMcpToolDispatchResult Result = Queue.Submit(
				TEXT("agent-x"), TEXT("unreal_open_mcp_echo"),
				[]() -> FUnrealOpenMcpToolDispatchResult
				{
					return FUnrealOpenMcpToolDispatchResult::Ok(TEXT("\"hello\""));
				});
			TestTrue(TEXT("Ok"), Result.bOk);
			TestEqual(TEXT("output"), Result.Output, FString(TEXT("\"hello\"")));
			TestEqual(TEXT("queue drained"), Queue.PendingCount(), 0);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
