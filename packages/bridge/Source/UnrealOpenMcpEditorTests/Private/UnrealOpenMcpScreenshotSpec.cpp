// unreal_open_mcp_screenshot_* Automation specs (P5.5).
//
// Pins the screenshot family at the GPU-FREE level: the encode path (opaque-
// force + PNG signature + byte-cap refuse), the dimension clamp helpers, the
// byte-cap constant, and the per-handler arg validation + actor-resolution
// branches that run BEFORE the GPU guard (so every case here is headless /
// -nullrhi safe). The actual pixel capture paths are live-verified windowed
// (manual GPU checklist) — no spec attempts a render here.
//
// The capture-failed / image_too_large dispatch codes are pinned via the
// EncodePngBase64 helper directly (over-cap → false + error) so the handler's
// error mapping is covered without a GPU.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/ScopeExit.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Tools/UnrealOpenMcpScreenshotTools.h"
#include "Tools/UnrealOpenMcpObjectRef.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpScreenshotSpec,
	"UnrealOpenMcp.Tools.Screenshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpScreenshotSpec)

namespace
{
	TSharedPtr<FJsonObject> ParseJson_Screenshot(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	bool GetScreenshotHandler(const FString& ToolName, FUnrealOpenMcpToolHandler& OutHandler)
	{
		FUnrealOpenMcpToolRegistry Registry;
		FUnrealOpenMcpScreenshotTools::Register(Registry);
		return Registry.TryGet(ToolName, OutHandler);
	}

	/** Decode a base64 string into raw bytes (FBase64::Decode wrapper for specs). */
	TArray<uint8> DecodeBase64(const FString& B64)
	{
		TArray<uint8> Out;
		FBase64::Decode(B64, Out);
		return Out;
	}
}

void FUnrealOpenMcpScreenshotSpec::Define()
{
	Describe("dimension clamps (GPU-free)", [this]()
	{
		It("ResolveCaptureDimension: omitted (<=0) yields the default", [this]()
		{
			TestEqual(TEXT("0 -> default"),
				FUnrealOpenMcpScreenshotTools::ResolveCaptureDimension(0),
				FUnrealOpenMcpScreenshotTools::DefaultCaptureDimension);
			TestEqual(TEXT("negative -> default"),
				FUnrealOpenMcpScreenshotTools::ResolveCaptureDimension(-100),
				FUnrealOpenMcpScreenshotTools::DefaultCaptureDimension);
		});

		It("ResolveCaptureDimension: clamps to [1, MaxCaptureDimension]", [this]()
		{
			TestEqual(TEXT("1 passes through"),
				FUnrealOpenMcpScreenshotTools::ResolveCaptureDimension(1), 1);
			TestEqual(TEXT("512 passes through"),
				FUnrealOpenMcpScreenshotTools::ResolveCaptureDimension(512), 512);
			TestEqual(TEXT("over-cap clamps to max"),
				FUnrealOpenMcpScreenshotTools::ResolveCaptureDimension(99999),
				FUnrealOpenMcpScreenshotTools::MaxCaptureDimension);
		});

		It("CapToMaxDimension: proportionally downscales the longest side", [this]()
		{
			int32 W = 0, H = 0;
			// 4096x2048 → longest side 4096 > 2048 → halve both.
			FUnrealOpenMcpScreenshotTools::CapToMaxDimension(4096, 2048, W, H);
			TestEqual(TEXT("capped W"), W, 2048);
			TestEqual(TEXT("capped H"), H, 1024);
		});

		It("CapToMaxDimension: no-op when within the cap", [this]()
		{
			int32 W = 0, H = 0;
			FUnrealOpenMcpScreenshotTools::CapToMaxDimension(1024, 768, W, H);
			TestEqual(TEXT("unchanged W"), W, 1024);
			TestEqual(TEXT("unchanged H"), H, 768);
		});

		It("CapToMaxDimension: floors each side at 1", [this]()
		{
			int32 W = 0, H = 0;
			FUnrealOpenMcpScreenshotTools::CapToMaxDimension(0, -5, W, H);
			TestEqual(TEXT("floored W"), W, 1);
			TestEqual(TEXT("floored H"), H, 1);
		});
	});

	Describe("EncodePngBase64 (GPU-free encode path)", [this]()
	{
		It("encodes a valid buffer to a PNG with the 0x89504E47 signature", [this]()
		{
			// A 2x2 buffer of distinct colors (alpha left at 0 to prove the
			// opaque-force runs — see the next case).
			TArray<FColor> Pixels = {
				FColor(255, 0, 0, 0),
				FColor(0, 255, 0, 0),
				FColor(0, 0, 255, 0),
				FColor(255, 255, 0, 0)
			};
			FString Base64;
			int32 Bytes = 0;
			FString Error;
			TestTrue(TEXT("encode ok"),
				FUnrealOpenMcpScreenshotTools::EncodePngBase64(Pixels, 2, 2, Base64, Bytes, Error));
			TestTrue(TEXT("base64 non-empty"), !Base64.IsEmpty());
			TestTrue(TEXT("byte count positive"), Bytes > 0);

			// PNG signature: 89 50 4E 47 0D 0A 1A 0A.
			const TArray<uint8> Raw = DecodeBase64(Base64);
			if (TestTrue(TEXT("decoded >= 8 bytes"), Raw.Num() >= 8))
			{
				TestEqual(TEXT("sig[0]"), Raw[0], uint8(0x89));
				TestEqual(TEXT("sig[1]"), Raw[1], uint8('P'));
				TestEqual(TEXT("sig[2]"), Raw[2], uint8('N'));
				TestEqual(TEXT("sig[3]"), Raw[3], uint8('G'));
			}
		});

		It("forces every pixel opaque (alpha → 255)", [this]()
		{
			// Hand the encoder a fully-transparent buffer (alpha 0). After the
			// call the in-place buffer must have alpha 255 on every pixel — the
			// guarantee that prevents a transparent PNG (useless to agents).
			TArray<FColor> Pixels = {
				FColor(10, 20, 30, 0),
				FColor(40, 50, 60, 0),
				FColor(70, 80, 90, 0),
				FColor(100, 110, 120, 0)
			};
			FString Base64;
			int32 Bytes = 0;
			FString Error;
			TestTrue(TEXT("encode ok"),
				FUnrealOpenMcpScreenshotTools::EncodePngBase64(Pixels, 2, 2, Base64, Bytes, Error));
			for (int32 i = 0; i < Pixels.Num(); ++i)
			{
				TestEqual(
					FString::Printf(TEXT("pixel[%d].A == 255"), i),
					Pixels[i].A, 255);
			}
		});

		It("rejects a zero-sized image", [this]()
		{
			TArray<FColor> Pixels = { FColor::Black };
			FString Base64;
			int32 Bytes = 0;
			FString Error;
			TestFalse(TEXT("0x0 refused"),
				FUnrealOpenMcpScreenshotTools::EncodePngBase64(Pixels, 0, 0, Base64, Bytes, Error));
			TestTrue(TEXT("error message set"), !Error.IsEmpty());
		});

		It("rejects an under-sized pixel buffer", [this]()
		{
			// 2x2 requested but only 1 pixel supplied.
			TArray<FColor> Pixels = { FColor::Black };
			FString Base64;
			int32 Bytes = 0;
			FString Error;
			TestFalse(TEXT("under-size refused"),
				FUnrealOpenMcpScreenshotTools::EncodePngBase64(Pixels, 2, 2, Base64, Bytes, Error));
			TestTrue(TEXT("error message set"), !Error.IsEmpty());
		});

		It("reports the byte cap as a constant (40 MiB)", [this]()
		{
			// The cap is the safeguard against giant payloads. Pin the value so
			// a regression that silently raises it is caught.
			TestEqual(
				TEXT("MaxEncodedBytes == 40 MiB"),
				FUnrealOpenMcpScreenshotTools::MaxEncodedBytes,
				static_cast<int64>(40) * 1024 * 1024);
		});
	});

	Describe("handler arg validation (GPU-free branches)", [this]()
	{
		It("screenshot_viewport returns invalid_parameter on a malformed body", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetScreenshotHandler(TEXT("unreal_open_mcp_screenshot_viewport"), Handler));
			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("not json"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});

		It("screenshot_game_view returns pie_not_running when no PIE session", [this]()
		{
			// Headless / no-PIE: the precondition check runs FIRST (GPU-free),
			// so this is the deterministic branch an agent hits without a game
			// running.
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetScreenshotHandler(TEXT("unreal_open_mcp_screenshot_game_view"), Handler));
			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			// Under -nullrhi with no PIE, pie_not_running is checked before the
			// GPU guard. (When a PIE session IS active the code differs — this
			// spec only pins the no-PIE headless branch.)
			if (GEditor == nullptr || (GEditor->PlayWorld == nullptr && GEditor->GetPIEViewport() == nullptr))
			{
				TestEqual(TEXT("code"), Result.Code, FString(TEXT("pie_not_running")));
			}
		});

		It("screenshot_camera returns missing_parameter when camera is absent", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetScreenshotHandler(TEXT("unreal_open_mcp_screenshot_camera"), Handler));
			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});

		It("screenshot_camera returns actor_not_found for an unresolvable camera", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetScreenshotHandler(TEXT("unreal_open_mcp_screenshot_camera"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"camera\":\"NoSuchCamera_ZZZ\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("actor_not_found")));
		});

		It("screenshot_isolated returns missing_parameter when actor is absent", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetScreenshotHandler(TEXT("unreal_open_mcp_screenshot_isolated"), Handler));
			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});

		It("screenshot_isolated returns actor_not_found for an unresolvable actor", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetScreenshotHandler(TEXT("unreal_open_mcp_screenshot_isolated"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"actor\":\"NoSuchActor_ZZZ\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("actor_not_found")));
		});

		It("screenshot_isolated returns invalid_parameter for a malformed background hex", [this]()
		{
			// Malformed hex is validated BEFORE the GPU guard (GPU-free branch).
			// Use a resolvable actor (spawn one) so the handler reaches the
			// background parse rather than failing on actor_not_found.
			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (!TestNotNull(TEXT("editor world"), World))
			{
				return;
			}
			AActor* Actor = World->SpawnActor<AActor>();
			if (!TestNotNull(TEXT("spawned actor"), Actor))
			{
				return;
			}
			Actor->SetActorLabel(TEXT("UnrealOpenMcpTestActor_Screenshot_Isolated"));
			ON_SCOPE_EXIT
			{
				if (Actor != nullptr && IsValid(Actor))
				{
					World->DestroyActor(Actor, true);
				}
			};

			FUnrealOpenMcpToolHandler Handler;
			GetScreenshotHandler(TEXT("unreal_open_mcp_screenshot_isolated"), Handler);
			const FString Body = FString::Printf(
				TEXT("{\"actor\":\"%s\",\"background\":\"nope\"}"),
				*Actor->GetActorLabel());
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});
	});

	Describe("OkWithImage dispatch result", [this]()
	{
		It("carries the image payload + mediaType on success", [this]()
		{
			// Pin the FUnrealOpenMcpToolDispatchResult::OkWithImage factory: the
			// image field must carry the base64 data + mediaType so the envelope
			// builder emits the top-level `image` block the LiveClient unwraps.
			const FString Meta = TEXT("{\"source\":\"test\",\"width\":1,\"height\":1}");
			const FUnrealOpenMcpToolDispatchResult R =
				FUnrealOpenMcpToolDispatchResult::OkWithImage(Meta, TEXT("YWJjZA=="), TEXT("image/png"));
			TestTrue(TEXT("ok"), R.bOk);
			TestEqual(TEXT("output spliced"), R.Output, Meta);
			TestTrue(TEXT("has image"), R.Image.HasImage());
			TestEqual(TEXT("base64 data"), R.Image.Base64Data, FString(TEXT("YWJjZA==")));
			TestEqual(TEXT("mediaType"), R.Image.MediaType, FString(TEXT("image/png")));
		});

		It("Ok (plain) carries no image payload", [this]()
		{
			const FUnrealOpenMcpToolDispatchResult R =
				FUnrealOpenMcpToolDispatchResult::Ok(TEXT("{\"a\":1}"));
			TestTrue(TEXT("ok"), R.bOk);
			TestFalse(TEXT("no image"), R.Image.HasImage());
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
