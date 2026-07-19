// unreal_open_mcp_asset_find / asset_get_data Automation specs (P4.1).
//
// Pins the AssetRegistry read spine end-to-end at the handler level (the
// dispatch spine is already pinned by earlier specs). The cases mirror the
// P4.1 plan's acceptance criteria:
//   - asset_find: empty filter defaults to /Game (recursive) and returns
//     bounded, stably ordered results with { total, offset, count };
//     pagination truncation (offset+limit); class-path validation rejects a
//     short (dotless) class name with invalid_class_path BEFORE the engine
//     ensure; a `tag_value` without `tag_key` is a missing_parameter error.
//   - asset_get_data: returns { name, path, package, class, tags } for a
//     known engine asset; missing asset → asset_not_found; missing `path` →
//     missing_parameter; `paths` field projection emits only the named
//     branches.
//   - HTTP round-trip: POST /tools/unreal_open_mcp_asset_find through a real
//     loopback socket returns the canonical {ok,result} envelope. This makes
//     asset_find a valid P4.5 smoke candidate.
//
// Disk-free by design: the cases read against the engine's shipped content
// (/Game/BasicShapes/Cube.Cube, the engine's default material at
// /Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial) so nothing is
// written or spawned. The cases probe for asset existence up front and skip
// (with a pass) when the automation project does not ship the expected
// content, so the suite is robust across minimal test projects.
//
// Adapted from Unity's search-assets / read-assets test surface at adapt
// fidelity: AssetRegistry live queries replace Unity's offline Assets/ scan;
// /Game + /Engine content paths replace Assets/ + Library/; pagination is
// offset/limit (Unreal-MCP style) rather than Unity's profile/cursor.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Bridge/UnrealOpenMcpBridgeHttpServer.h"
#include "Bridge/UnrealOpenMcpBridgeRequestQueue.h"
#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Dispatch/UnrealOpenMcpGameThreadDispatcher.h"
#include "Tools/UnrealOpenMcpAssetTools.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
// EditorAssetLibrary mirrors the production handler's path-or-name probe.
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"

#include "Common/TcpSocketBuilder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpAssetReadSpec,
	"UnrealOpenMcp.Tools.AssetRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpAssetReadSpec)

namespace
{
	/** A known engine asset that ships with every Unreal project — the engine's
	 *  default grid material. Lives under /Engine so it is always present
	 *  regardless of project content. Used as the get-data fixture. */
	constexpr const TCHAR* KnownEngineAssetPath =
		TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");

	/** A known /Game asset — the engine's default cube static mesh, present in
	 *  projects that ship the BasicShapes content pack. Used to exercise a /Game
	 *  find; cases probe for its existence up front and skip (with a pass) when
	 *  the automation project does not ship BasicShapes. */
	constexpr const TCHAR* KnownGameAssetPath = TEXT("/Game/BasicShapes/Cube.Cube");

	/** Parse a JSON object from a string. Null on failure. */
	TSharedPtr<FJsonObject> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	/** Send a raw HTTP POST and read the full response until the server closes.
	 *  Same shape as the actor-find / level-inspect specs' SendHttpPost. */
	FString SendHttpPost(
		uint16 Port,
		const FString& Path,
		const FString& Body)
	{
		const FIPv4Address Loopback(127, 0, 0, 1);
		const FIPv4Endpoint Endpoint(Loopback, Port);

		FSocket* Client = FTcpSocketBuilder(TEXT("UnrealOpenMcpAssetReadTestClient"))
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

	FString ExtractHttpBody(const FString& Response)
	{
		const int32 Separator = Response.Find(TEXT("\r\n\r\n"));
		return Separator == INDEX_NONE ? Response : Response.Mid(Separator + 4);
	}

	/** True when @p AssetPath exists in the AssetRegistry. Used to gate the
	 *  /Game fixture cases so they skip (with a pass) when the project does
	 *  not ship the expected content. */
	bool AssetExists(const FString& AssetPath)
	{
		return UEditorAssetLibrary::DoesAssetExist(AssetPath);
	}

	/** Ephemeral dispatch harness wired with the asset tools registered.
	 *  Mirrors the actor-find / level-inspect FDispatchHarness; lives on the
	 *  stack so teardown is server-first. */
	struct FDispatchHarness
	{
		FUnrealOpenMcpGameThreadDispatcher Dispatcher;
		FUnrealOpenMcpToolRegistry Registry;
		FUnrealOpenMcpBridgeRequestQueue Queue;
		FUnrealOpenMcpBridgeHttpServer* Server = nullptr;

		bool Start(const FString& ProjectPath)
		{
			FUnrealOpenMcpAssetTools::Register(Registry);
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

void FUnrealOpenMcpAssetReadSpec::Define()
{
	Describe("unreal_open_mcp_asset_find — handler contract", [this]()
	{
		// Empty filter defaults to /Game (recursive) — never the whole registry
		// incl. /Engine. The result is bounded and carries { total, offset,
		// count }. Even when /Game is empty (minimal test project), the call
		// returns { total: 0 } with a stable shape — not an error.
		It("empty filter defaults to /Game and returns the bounded shape", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), Registry.TryGet(TEXT("unreal_open_mcp_asset_find"), Handler));

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			// Stable shape: { total, offset, count, assets[] }.
			TestTrue(TEXT("has total"), Json->HasField(TEXT("total")));
			TestEqual(TEXT("offset defaults to 0"), static_cast<int32>(Json->GetNumberField(TEXT("offset"))), 0);
			TestTrue(TEXT("has count"), Json->HasField(TEXT("count")));
			TestTrue(TEXT("has assets array"), Json->HasTypedField<EJson::Array>(TEXT("assets")));
			const int32 Total = static_cast<int32>(Json->GetNumberField(TEXT("total")));
			const int32 Count = static_cast<int32>(Json->GetNumberField(TEXT("count")));
			TestEqual(TEXT("count == assets.length"), Count, Json->GetArrayField(TEXT("assets")).Num());
			TestTrue(TEXT("count <= total"), Count <= Total);
		});

		// Pagination: offset+limit slices the result window. Pin the contract
		// by enumerating the engine materials class (always present) and
		// asking for two pages; the second page's offset must match.
		It("offset+limit paginates the result window", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_asset_find"), Handler);

			// Page 1: first 2 engine materials.
			const FUnrealOpenMcpToolDispatchResult P1 = Handler(
				TEXT("{\"class_path\":\"/Script/Engine.Material\",\"path\":\"/Engine\",\"limit\":2,\"offset\":0}"));
			if (!TestTrue(TEXT("page1 ok"), P1.bOk)) return;
			const TSharedPtr<FJsonObject> J1 = ParseJson(P1.Output);
			if (!TestNotNull(TEXT("page1 json"), J1.Get())) return;
			const int32 Total = static_cast<int32>(J1->GetNumberField(TEXT("total")));
			if (!TestTrue(TEXT("engine has >= 2 materials"), Total >= 2)) return;
			TestEqual(TEXT("page1 count"), static_cast<int32>(J1->GetNumberField(TEXT("count"))), 2);
			TestEqual(TEXT("page1 offset"), static_cast<int32>(J1->GetNumberField(TEXT("offset"))), 0);

			// Page 2: next 2. The offset reflects the request.
			const FUnrealOpenMcpToolDispatchResult P2 = Handler(
				TEXT("{\"class_path\":\"/Script/Engine.Material\",\"path\":\"/Engine\",\"limit\":2,\"offset\":2}"));
			if (!TestTrue(TEXT("page2 ok"), P2.bOk)) return;
			const TSharedPtr<FJsonObject> J2 = ParseJson(P2.Output);
			if (!TestNotNull(TEXT("page2 json"), J2.Get())) return;
			TestEqual(TEXT("page2 offset"), static_cast<int32>(J2->GetNumberField(TEXT("offset"))), 2);
			TestEqual(TEXT("page2 total stable"), static_cast<int32>(J2->GetNumberField(TEXT("total"))), Total);

			// Stable ordering: the first page's entries sort strictly before
			// the second page's entries by object path.
			const TArray<TSharedPtr<FJsonValue>>& A1 = J1->GetArrayField(TEXT("assets"));
			const TArray<TSharedPtr<FJsonValue>>& A2 = J2->GetArrayField(TEXT("assets"));
			if (A1.Num() > 0 && A2.Num() > 0)
			{
				const FString LastOfP1 = A1.Last()->AsObject()->GetStringField(TEXT("path"));
				const FString FirstOfP2 = A2[0]->AsObject()->GetStringField(TEXT("path"));
				TestTrue(
					TEXT("stable ordering across pages"),
					LastOfP1.Compare(FirstOfP2, ESearchCase::CaseSensitive) < 0);
			}
		});

		// Invalid class path — a short (dotless) name would fire an engine
		// ensure inside FTopLevelAssetPath construction. Validate BEFORE and
		// return invalid_class_path.
		It("rejects a short class name with invalid_class_path", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_asset_find"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"class_path\":\"Material\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_class_path")));
		});

		// Class path missing the leading slash is also invalid.
		It("rejects a class path without a leading slash", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_asset_find"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"class_path\":\"Script.Engine.Material\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_class_path")));
		});

		// tag_value without tag_key is a no-op in the registry filter; surface
		// as missing_parameter so the caller's intent is not silently dropped.
		It("tag_value without tag_key returns missing_parameter", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_asset_find"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"tag_value\":\"Foo\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});

		// Malformed body → invalid_parameter.
		It("returns invalid_parameter for a malformed body", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_asset_find"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("<<<not json>>>"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});
	});

	Describe("unreal_open_mcp_asset_get_data — handler contract", [this]()
	{
		// Happy path: read a known engine asset. The result carries the
		// AssetSummary block { name, path, package, class } plus a tags map
		// (possibly empty).
		It("returns name/path/package/class/tags for a known asset", [this]()
		{
			if (!TestTrue(TEXT("fixture asset exists"), AssetExists(KnownEngineAssetPath)))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_asset_get_data"), Handler);

			const FString Body = FString::Printf(TEXT("{\"path\":\"%s\"}"), KnownEngineAssetPath);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestTrue(TEXT("has name"), Json->HasTypedField<EJson::String>(TEXT("name")));
			TestTrue(TEXT("has path"), Json->HasTypedField<EJson::String>(TEXT("path")));
			TestTrue(TEXT("has package"), Json->HasTypedField<EJson::String>(TEXT("package")));
			TestTrue(TEXT("has class"), Json->HasTypedField<EJson::String>(TEXT("class")));
			TestTrue(TEXT("has tags map"), Json->HasTypedField<EJson::Object>(TEXT("tags")));
		});

		// Path-or-name: a package path (no trailing .AssetName) is accepted.
		It("accepts a package-path form (no trailing .AssetName)", [this]()
		{
			if (!TestTrue(TEXT("fixture asset exists"), AssetExists(KnownEngineAssetPath)))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_asset_get_data"), Handler);

			// Strip the trailing ".WorldGridMaterial" → /Engine/EngineMaterials/WorldGridMaterial
			FString PackageForm = KnownEngineAssetPath;
			int32 Dot;
			if (PackageForm.FindChar(TEXT('.'), Dot))
			{
				PackageForm.LeftInline(Dot);
			}
			const FString Body = FString::Printf(TEXT("{\"path\":\"%s\"}"), *PackageForm);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);
		});

		// Missing asset → asset_not_found (not a crash, not an empty payload).
		It("returns asset_not_found for a missing asset", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_asset_get_data"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"path\":\"/Game/Does/Not/Exist.Nope\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("asset_not_found")));
		});

		// Missing `path` → missing_parameter.
		It("returns missing_parameter when path is absent", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_asset_get_data"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});

		// Field projection: `paths` returns only the named branches.
		It("paths projection emits only the named branches", [this]()
		{
			if (!TestTrue(TEXT("fixture asset exists"), AssetExists(KnownEngineAssetPath)))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_asset_get_data"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"path\":\"%s\",\"paths\":[\"name\",\"class\"]}"),
				KnownEngineAssetPath);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestTrue(TEXT("name present"), Json->HasTypedField<EJson::String>(TEXT("name")));
			TestTrue(TEXT("class present"), Json->HasTypedField<EJson::String>(TEXT("class")));
			// Projected branches NOT named in `paths` are omitted.
			TestFalse(TEXT("path omitted"), Json->HasField(TEXT("path")));
			TestFalse(TEXT("package omitted"), Json->HasField(TEXT("package")));
			TestFalse(TEXT("tags omitted"), Json->HasField(TEXT("tags")));
		});

		// `paths` not-an-array → invalid_parameter (refuse rather than widen).
		It("paths as a non-array returns invalid_parameter", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_asset_get_data"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"path\":\"/Game/Foo.Foo\",\"paths\":\"name\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});

		// Malformed body → invalid_parameter.
		It("returns invalid_parameter for a malformed body", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_asset_get_data"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("<<<not json>>>"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});
	});

	Describe("unreal_open_mcp_asset_find — /Game fixture", [this]()
	{
		// A /Game-scoped find by class returns the expected AssetSummary shape
		// for each entry. Gated on the BasicShapes content pack being present.
		It("class_path + path returns AssetSummary entries", [this]()
		{
			if (!TestTrue(TEXT("/Game fixture exists"), AssetExists(KnownGameAssetPath)))
			{
				return;
			}

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpAssetTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_asset_find"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"class_path\":\"/Script/Engine.StaticMesh\",\"path\":\"/Game/BasicShapes\"}"));
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestTrue(TEXT("total >= 1"), static_cast<int32>(Json->GetNumberField(TEXT("total"))) >= 1);
			const TArray<TSharedPtr<FJsonValue>>& Assets = Json->GetArrayField(TEXT("assets"));
			if (Assets.Num() > 0)
			{
				const TSharedPtr<FJsonObject> Entry = Assets[0]->AsObject;
				TestTrue(TEXT("entry name"), Entry->HasTypedField<EJson::String>(TEXT("name")));
				TestTrue(TEXT("entry path"), Entry->HasTypedField<EJson::String>(TEXT("path")));
				TestTrue(TEXT("entry package"), Entry->HasTypedField<EJson::String>(TEXT("package")));
				TestTrue(TEXT("entry class"), Entry->HasTypedField<EJson::String>(TEXT("class")));
			}
		});
	});

	Describe("unreal_open_mcp_asset_find — HTTP round-trip", [this]()
	{
		// Full POST → registry → game-thread handler → {ok,result} envelope on
		// a real loopback socket. This is the path P4.5 smoke exercises.
		LatentIt(
			"returns the {ok,result} envelope over POST /tools/{name}",
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

				const FString Response = SendHttpPost(
					Harness.Server->GetPort(),
					TEXT("/tools/unreal_open_mcp_asset_find"),
					TEXT("{}"));
				Harness.Stop();

				TestTrue(TEXT("HTTP body present"), Response.Contains(TEXT("HTTP/")));
				const FString Body = ExtractHttpBody(Response);
				TestTrue(TEXT("ok:true"), Body.Contains(TEXT("\"ok\":true")));
				TestTrue(TEXT("total field"), Body.Contains(TEXT("\"total\"")));
				TestTrue(TEXT("count field"), Body.Contains(TEXT("\"count\"")));
				TestTrue(TEXT("assets field"), Body.Contains(TEXT("\"assets\"")));
				Done.Execute();
			});
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
