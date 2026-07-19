// unreal_open_mcp_editor_application_get_state / _set_state Automation specs
// (P5.1).
//
// Pins the editor application state pair end-to-end at the handler level (the
// dispatch spine is already pinned by earlier specs), plus one HTTP round-trip
// for the read-only get-state envelope.
//
// Deterministic-by-design: the Automation editor is NOT in a Play-In-Editor
// session, so every case here exercises the NOT-PLAYING state — the get-state
// DTO shape (all-false flags + an editorMap string) and the invalid-transition
// surface (stop / pause / resume with no session active), plus the arg-shape
// errors (missing / non-enum action). The `start` happy-path would actually
// launch PIE (a heavy, non-deterministic game loop), so it is a manual
// verification item (see the P5.1 plan) and is intentionally NOT driven here —
// the acceptance-critical guarantees this spec pins are the honest error /
// snapshot contracts, not the live PIE launch.
//
// Adapted from Unity's editor-status / editor-set-state tests: the field set is
// the Unreal PIE trio (isPlaying / isPaused / isSimulating) rather than Unity's
// is_playing boolean, and the transitions are the action enum (start/stop/
// pause/resume) with a latent `pending` result.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Bridge/UnrealOpenMcpBridgeHttpServer.h"
#include "Bridge/UnrealOpenMcpBridgeRequestQueue.h"
#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Dispatch/UnrealOpenMcpGameThreadDispatcher.h"
#include "Tools/UnrealOpenMcpEditorTools.h"
#include "Tools/UnrealOpenMcpObjectRef.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Engine/World.h"
#include "Editor.h"                 // GEditor

#include "Common/TcpSocketBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpEditorApplicationStateSpec,
	"UnrealOpenMcp.Tools.EditorApplicationState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpEditorApplicationStateSpec)

namespace
{
	/** Parse a JSON object from a string. Null on failure. */
	TSharedPtr<FJsonObject> ParseJson_EditorState(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	/** Resolve an editor tool handler by name from a fresh registry. */
	bool GetEditorHandler_State(const FString& ToolName, FUnrealOpenMcpToolHandler& OutHandler)
	{
		FUnrealOpenMcpToolRegistry Registry;
		FUnrealOpenMcpEditorTools::Register(Registry);
		return Registry.TryGet(ToolName, OutHandler);
	}

	/** Send a raw HTTP POST and read the full response until the server closes.
	 *  Same shape as the level-inspect spec's SendHttpPost. */
	FString SendHttpPost_EditorState(uint16 Port, const FString& Path, const FString& Body)
	{
		const FIPv4Address Loopback(127, 0, 0, 1);
		const FIPv4Endpoint Endpoint(Loopback, Port);

		FSocket* Client = FTcpSocketBuilder(TEXT("UnrealOpenMcpEditorStateTestClient"))
			.AsBlocking()
			.Build();
		if (Client == nullptr)
		{
			return FString();
		}
		Client->Connect(*Endpoint.ToInternetAddr());

		const FTCHARToUTF8 BodyUtf8(*Body);
		FString Request = FString::Printf(
			TEXT("POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"),
			*Path);
		Request += FString::Printf(TEXT("Content-Length: %d\r\n"), BodyUtf8.Length());
		Request += TEXT("Content-Type: application/json; charset=utf-8\r\n\r\n");

		const FTCHARToUTF8 RequestUtf8(*Request);
		int32 Sent = 0;
		Client->Send(reinterpret_cast<const uint8*>(RequestUtf8.Get()), RequestUtf8.Length(), Sent);
		Client->Send(reinterpret_cast<const uint8*>(BodyUtf8.Get()), BodyUtf8.Length(), Sent);

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

	FString ExtractHttpBody_EditorState(const FString& Response)
	{
		const int32 Separator = Response.Find(TEXT("\r\n\r\n"));
		return Separator == INDEX_NONE ? Response : Response.Mid(Separator + 4);
	}

	/** Ephemeral dispatch harness wired with the editor tools registered. */
	struct FEditorStateHarness
	{
		FUnrealOpenMcpGameThreadDispatcher Dispatcher;
		FUnrealOpenMcpToolRegistry Registry;
		FUnrealOpenMcpBridgeRequestQueue Queue;
		FUnrealOpenMcpBridgeHttpServer* Server = nullptr;

		bool Start(const FString& ProjectPath)
		{
			FUnrealOpenMcpEditorTools::Register(Registry);
			Server = new FUnrealOpenMcpBridgeHttpServer(Dispatcher, Registry, Queue);
			return Server->Start(0, ProjectPath);
		}

		void Stop()
		{
			if (Server != nullptr)
			{
				Server->Stop();
				delete Server;
				Server = nullptr;
			}
		}
	};
}

void FUnrealOpenMcpEditorApplicationStateSpec::Define()
{
	Describe("unreal_open_mcp_editor_application_get_state — handler contract", [this]()
	{
		// The Automation editor is not in PIE: the flags are all false and the
		// editorMap / editorMapName strings are present (identifying the current
		// editing world).
		It("reports the not-playing DTO with all flags false + an editorMap", [this]()
		{
			if (!TestNotNull(TEXT("editor world"), FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetEditorHandler_State(TEXT("unreal_open_mcp_editor_application_get_state"), Handler));

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson_EditorState(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestTrue(TEXT("has isPlaying"), Json->HasTypedField<EJson::Boolean>(TEXT("isPlaying")));
			TestTrue(TEXT("has isPaused"), Json->HasTypedField<EJson::Boolean>(TEXT("isPaused")));
			TestTrue(TEXT("has isSimulating"), Json->HasTypedField<EJson::Boolean>(TEXT("isSimulating")));
			// Not in PIE → every flag false.
			TestFalse(TEXT("isPlaying false"), Json->GetBoolField(TEXT("isPlaying")));
			TestFalse(TEXT("isPaused false"), Json->GetBoolField(TEXT("isPaused")));
			TestFalse(TEXT("isSimulating false"), Json->GetBoolField(TEXT("isSimulating")));
			// editorMap identifies the editing world.
			TestTrue(TEXT("has editorMap"), Json->HasTypedField<EJson::String>(TEXT("editorMap")));
			TestFalse(TEXT("editorMap non-empty"), Json->GetStringField(TEXT("editorMap")).IsEmpty());
			TestTrue(TEXT("has editorMapName"), Json->HasTypedField<EJson::String>(TEXT("editorMapName")));
		});
	});

	Describe("unreal_open_mcp_editor_application_set_state — handler contract", [this]()
	{
		// Missing action → missing_parameter.
		It("returns missing_parameter when action is absent", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetEditorHandler_State(TEXT("unreal_open_mcp_editor_application_set_state"), Handler));

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});

		// Unknown action value → invalid_parameter.
		It("returns invalid_parameter for an unknown action", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetEditorHandler_State(TEXT("unreal_open_mcp_editor_application_set_state"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{\"action\":\"launch\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});

		// stop while not playing → invalid_transition (nothing to stop).
		It("returns invalid_transition on stop when no session is active", [this]()
		{
			// Guard: this determinism holds only when not already in PIE (the
			// Automation editor is not).
			if (GEditor != nullptr && GEditor->PlayWorld != nullptr)
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			GetEditorHandler_State(TEXT("unreal_open_mcp_editor_application_set_state"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{\"action\":\"stop\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_transition")));
		});

		// pause while not playing → invalid_transition.
		It("returns invalid_transition on pause when no session is active", [this]()
		{
			if (GEditor != nullptr && GEditor->PlayWorld != nullptr)
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			GetEditorHandler_State(TEXT("unreal_open_mcp_editor_application_set_state"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{\"action\":\"pause\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_transition")));
		});

		// resume while not playing → invalid_transition.
		It("returns invalid_transition on resume when no session is active", [this]()
		{
			if (GEditor != nullptr && GEditor->PlayWorld != nullptr)
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			GetEditorHandler_State(TEXT("unreal_open_mcp_editor_application_set_state"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{\"action\":\"resume\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_transition")));
		});
	});

	Describe("unreal_open_mcp_editor_application_get_state — HTTP round-trip", [this]()
	{
		// Full POST → registry → game-thread handler → {ok,result} envelope on a
		// real loopback socket. Read-only, so no paths_hint / gate involved.
		LatentIt(
			"returns the {ok,result} envelope with the PIE flags over POST /tools/{name}",
			FTimespan::FromSeconds(30),
			[this](const FDoneDelegate& Done)
			{
				Async(EAsyncExecution::Thread, [this, Done]()
				{
					FEditorStateHarness Harness;
					if (!TestTrue(TEXT("server started"), Harness.Start(TEXT("/tmp/test"))))
					{
						Done.Execute();
						return;
					}

					const FString Response = SendHttpPost_EditorState(
						Harness.Server->GetPort(),
						TEXT("/tools/unreal_open_mcp_editor_application_get_state"),
						TEXT("{}"));
					Harness.Stop();

					TestTrue(TEXT("HTTP body present"), Response.Contains(TEXT("HTTP/")));
					const FString RespBody = ExtractHttpBody_EditorState(Response);
					TestTrue(TEXT("ok:true"), RespBody.Contains(TEXT("\"ok\":true")));
					TestTrue(TEXT("isPlaying field"), RespBody.Contains(TEXT("\"isPlaying\"")));
					TestTrue(TEXT("editorMap field"), RespBody.Contains(TEXT("\"editorMap\"")));
					Done.Execute();
				});
			});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
