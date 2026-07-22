// Screenshot tool family — see header for the four-tool split, dimension caps,
// opaque-PNG rationale, and the GPU-free-validation-first ordering. Behavior
// reference (read-only): Unreal-MCP's screenshot handlers.
#include "Tools/UnrealOpenMcpScreenshotTools.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Tools/UnrealOpenMcpObjectRef.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/ScopeExit.h"
#include "UObject/GCObjectScopeGuard.h"

#include "Editor.h"
#include "UnrealClient.h"               // FViewport
#include "ImageUtils.h"                 // FImageUtils::PNGCompressImageArray / ImageResize
#include "TextureResource.h"            // FTextureRenderTargetResource::ReadPixels

#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/SceneCapture2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Camera/CameraComponent.h"

namespace
{
	/** Parse the raw POST body into a JSON object (empty → empty object,
	 *  malformed → null). Same contract as the other tool families. */
	TSharedPtr<FJsonObject> ParseBody(const FString& Body)
	{
		const FString Trimmed = Body.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return MakeShared<FJsonObject>();
		}
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			return nullptr;
		}
		return Object;
	}

	/** Serialize a JsonValue to a compact string ("null" on null). */
	FString WriteJson(const TSharedPtr<FJsonValue>& JsonValue)
	{
		if (!JsonValue.IsValid())
		{
			return TEXT("null");
		}
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		if (FJsonSerializer::Serialize(JsonValue, Writer))
		{
			return Out;
		}
		return TEXT("null");
	}

	/** Read an optional integer field as int64 (0 when absent / wrong-typed). */
	int64 GetIntField(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name)
	{
		if (Args.IsValid() && Args->HasTypedField<EJson::Number>(Name))
		{
			return static_cast<int64>(Args->GetNumberField(Name));
		}
		return 0;
	}

	/** Read an optional number field as float (0 when absent / wrong-typed). */
	float GetNumberField(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name)
	{
		if (Args.IsValid() && Args->HasTypedField<EJson::Number>(Name))
		{
			return static_cast<float>(Args->GetNumberField(Name));
		}
		return 0.0f;
	}

	/** Read an optional string field (empty when absent / wrong-typed). */
	FString GetStringField(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name)
	{
		if (Args.IsValid() && Args->HasTypedField<EJson::String>(Name))
		{
			return Args->GetStringField(Name);
		}
		return FString();
	}

	// Keep the encoded payload well under the bridge IPC line cap — see
	// FUnrealOpenMcpScreenshotTools::MaxEncodedBytes (the exported constant the
	// spec asserts against).

	/** True when the editor can render; false under headless `-nullrhi`. The
	 *  GPU guard runs AFTER arg validation so malformed-arg / no-PIE / actor-
	 *  not-found branches stay headless-spec-covered. */
	bool EnsureRenderingAvailable(FString& OutError)
	{
		if (!FApp::CanEverRender())
		{
			OutError = TEXT("Rendering is unavailable (headless/-nullrhi). Screenshot capture requires a GPU-backed editor; run the editor windowed and retry.");
			return false;
		}
		return true;
	}

	/** The structured `result` metadata block every screenshot tool returns
	 *  alongside the image payload. */
	TSharedRef<FJsonObject> MakeStructured(const FString& Source, int32 Width, int32 Height, int32 EncodedBytes)
	{
		TSharedRef<FJsonObject> Structured = MakeShared<FJsonObject>();
		Structured->SetStringField(TEXT("source"), Source);
		Structured->SetNumberField(TEXT("width"), Width);
		Structured->SetNumberField(TEXT("height"), Height);
		Structured->SetStringField(TEXT("mimeType"), TEXT("image/png"));
		Structured->SetNumberField(TEXT("byteSize"), EncodedBytes);
		return Structured;
	}

	/** Resample a captured buffer to (DstW, DstH) when it differs from the
	 *  source size. Used only by the viewport/PIE path (native-size → requested). */
	void ResizeIfNeeded(TArray<FColor>& Pixels, int32 SrcW, int32 SrcH, int32 DstW, int32 DstH)
	{
		if ((SrcW == DstW && SrcH == DstH) || DstW <= 0 || DstH <= 0)
		{
			return;
		}
		TArray<FColor> Resized;
		Resized.SetNumUninitialized(DstW * DstH);
		FImageUtils::ImageResize(SrcW, SrcH, Pixels, DstW, DstH, Resized, /*bResizeSRGBinLinearSpace*/ false);
		Pixels = MoveTemp(Resized);
	}

	/**
	 * Effective output size for a viewport-style capture. When BOTH width and
	 * height are supplied they are each clamped to [1, 2048]; when only ONE is
	 * supplied the other is derived from the native aspect ratio (so a lone
	 * width=512 on a 1920x1080 viewport yields 512x288, not a stretched
	 * 512x1080); when NEITHER is supplied the native size is used. The result
	 * is then hard-capped.
	 */
	void EffectiveViewportSize(
		const TSharedPtr<FJsonObject>& Args,
		int32 NativeW, int32 NativeH, int32& OutW, int32& OutH)
	{
		const int64 ReqW = GetIntField(Args, TEXT("width"));
		const int64 ReqH = GetIntField(Args, TEXT("height"));
		int32 W, H;
		if (ReqW > 0 && ReqH > 0)
		{
			W = FUnrealOpenMcpScreenshotTools::ResolveCaptureDimension(ReqW);
			H = FUnrealOpenMcpScreenshotTools::ResolveCaptureDimension(ReqH);
		}
		else if (ReqW > 0)
		{
			W = FUnrealOpenMcpScreenshotTools::ResolveCaptureDimension(ReqW);
			H = NativeW > 0 ? FMath::Max(1, FMath::RoundToInt(W * (double)NativeH / (double)NativeW)) : NativeH;
		}
		else if (ReqH > 0)
		{
			H = FUnrealOpenMcpScreenshotTools::ResolveCaptureDimension(ReqH);
			W = NativeH > 0 ? FMath::Max(1, FMath::RoundToInt(H * (double)NativeW / (double)NativeH)) : NativeW;
		}
		else
		{
			W = NativeW;
			H = NativeH;
		}
		FUnrealOpenMcpScreenshotTools::CapToMaxDimension(W, H, OutW, OutH);
	}

	/** Build the success dispatch result carrying the PNG image payload. */
	FUnrealOpenMcpToolDispatchResult MakeImageResult(
		const FString& Source, int32 Width, int32 Height, int32 EncodedBytes,
		const FString& Base64)
	{
		const FString Output = WriteJson(MakeShared<FJsonValueObject>(MakeStructured(Source, Width, Height, EncodedBytes)));
		return FUnrealOpenMcpToolDispatchResult::OkWithImage(Output, Base64, TEXT("image/png"));
	}

	/** Read + encode an FViewport (editor or PIE). Called only on the
	 *  GPU-available path after arg validation + viewport resolution. */
	FUnrealOpenMcpToolDispatchResult CaptureFromViewport(
		const TSharedPtr<FJsonObject>& Args, FViewport* Viewport, const FString& Source)
	{
		const FIntPoint Size = Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			return FUnrealOpenMcpToolDispatchResult::Fail(
				TEXT("capture_failed"),
				FString::Printf(TEXT("The %s has a zero-sized render area."), *Source));
		}

		// Ensure a current frame is present before the read-back (a stale
		// backbuffer would otherwise yield empty pixels).
		Viewport->Draw();

		TArray<FColor> Pixels;
		if (!Viewport->ReadPixels(Pixels, FReadSurfaceDataFlags(), FIntRect(0, 0, Size.X, Size.Y)))
		{
			return FUnrealOpenMcpToolDispatchResult::Fail(
				TEXT("capture_failed"),
				FString::Printf(TEXT("Failed to read pixels from the %s."), *Source));
		}

		int32 OutW = 0, OutH = 0;
		EffectiveViewportSize(Args, Size.X, Size.Y, OutW, OutH);
		ResizeIfNeeded(Pixels, Size.X, Size.Y, OutW, OutH);

		FString Base64;
		int32 Bytes = 0;
		FString Error;
		if (!EncodePngBase64(Pixels, OutW, OutH, Base64, Bytes, Error))
		{
			return FUnrealOpenMcpToolDispatchResult::Fail(
				Bytes == 0 ? TEXT("capture_failed") : TEXT("image_too_large"), Error);
		}

		return MakeImageResult(Source, OutW, OutH, Bytes, Base64);
	}

	/**
	 * Render through a transient SceneCapture2D into a transient render target,
	 * then read back. The capture actor is spawned RF_Transient + hidden from
	 * the outliner and destroyed on every path (ON_SCOPE_EXIT), so the editor
	 * world is never dirtied. The render target lives on the transient package
	 * + an FGCObjectScopeGuard so it is GC'd at scope exit and never saved.
	 * Called only on the GPU-available path.
	 */
	FUnrealOpenMcpToolDispatchResult CaptureWithSceneCapture(
		UWorld* World, const FTransform& CaptureXform, float FovDeg, int32 Width, int32 Height,
		const FString& Source, const FLinearColor* BackgroundColor, AActor* ShowOnlyActor)
	{
		// Isolated mode (a background was requested) composites the actor over a
		// solid color. A plain SCS_FinalColorLDR capture writes opaque pixels
		// everywhere, overwriting the render target's ClearColor — so an empty
		// region renders as the scene's (black) background, NOT the requested
		// color. To honor the background we capture coverage-carrying HDR scene
		// color (SCS_SceneColorHDR, inverse opacity in alpha) into a float
		// target and composite scene-over-background in the read-back.
		// screenshot-camera passes no background and keeps the cheaper tonemapped
		// LDR path. (Behavior reference: Unreal-MCP CaptureWithSceneCapture.)
		const bool bComposite = (BackgroundColor != nullptr);

		UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		RenderTarget->RenderTargetFormat = bComposite ? RTF_RGBA16f : RTF_RGBA8;
		RenderTarget->ClearColor = BackgroundColor ? *BackgroundColor : FLinearColor::Black;
		RenderTarget->bAutoGenerateMips = false;
		RenderTarget->InitAutoFormat(Width, Height);
		RenderTarget->UpdateResourceImmediate(true);
		FGCObjectScopeGuard RenderTargetGuard(RenderTarget);

		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transient;
		SpawnParams.bTemporaryEditorActor = true;
		SpawnParams.bHideFromSceneOutliner = true;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		ASceneCapture2D* CaptureActor = World->SpawnActor<ASceneCapture2D>(
			ASceneCapture2D::StaticClass(), CaptureXform, SpawnParams);
		if (!CaptureActor)
		{
			return FUnrealOpenMcpToolDispatchResult::Fail(
				TEXT("capture_failed"),
				TEXT("Failed to spawn a transient SceneCapture2D actor."));
		}
		ON_SCOPE_EXIT
		{
			if (IsValid(CaptureActor))
			{
				CaptureActor->Destroy();
			}
		};

		USceneCaptureComponent2D* Capture = CaptureActor->GetCaptureComponent2D();
		if (!Capture)
		{
			return FUnrealOpenMcpToolDispatchResult::Fail(
				TEXT("capture_failed"),
				TEXT("Transient SceneCapture2D had no capture component."));
		}

		Capture->TextureTarget = RenderTarget;
		Capture->FOVAngle = FovDeg;
		Capture->CaptureSource = bComposite ? SCS_SceneColorHDR : SCS_FinalColorLDR;
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		// Pin auto-exposure. A single-shot CaptureScene() never gives eye-
		// adaptation a chance to converge, so the default auto-exposure leaves
		// one-off captures badly under/over-exposed. Locking min == max
		// brightness makes exposure a fixed factor (no adaptation transient),
		// so the capture is deterministic regardless of prior adaptation state.
		Capture->PostProcessSettings.bOverride_AutoExposureMinBrightness = true;
		Capture->PostProcessSettings.AutoExposureMinBrightness = 1.0f;
		Capture->PostProcessSettings.bOverride_AutoExposureMaxBrightness = true;
		Capture->PostProcessSettings.AutoExposureMaxBrightness = 1.0f;
		if (ShowOnlyActor)
		{
			Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
			Capture->ShowOnlyActors.Add(ShowOnlyActor);
			// Include attached child actors (child-actor components, attach
			// hierarchies — common for composed Blueprints) so visually-integral
			// parts are not silently omitted from the render.
			TArray<AActor*> AttachedActors;
			ShowOnlyActor->GetAttachedActors(AttachedActors, /*bResetArray*/ true, /*bRecursivelyIncludeAttachedActors*/ true);
			Capture->ShowOnlyActors.Append(AttachedActors);
		}
		// The capture component is the spawned actor's root (the actor was
		// spawned at CaptureXform), so its world transform already equals
		// CaptureXform — no explicit SetWorldLocationAndRotation needed.
		Capture->CaptureScene();

		FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
		if (!Resource)
		{
			return FUnrealOpenMcpToolDispatchResult::Fail(
				TEXT("capture_failed"),
				TEXT("Render target resource was not available after capture."));
		}

		TArray<FColor> Pixels;
		if (bComposite)
		{
			// SCS_SceneColorHDR stores inverse opacity in alpha (1 = empty/
			// background visible, 0 = opaque geometry), so compositing scene-
			// over-background recovers the requested solid background:
			//   final = sceneColor + background * alpha   (premultiplied "over",
			// done in linear space).
			TArray<FLinearColor> LinearPixels;
			if (!Resource->ReadLinearColorPixels(LinearPixels))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("capture_failed"),
					TEXT("Failed to read pixels from the capture render target."));
			}
			const FLinearColor Bg = *BackgroundColor;
			Pixels.SetNumUninitialized(LinearPixels.Num());
			for (int32 Index = 0; Index < LinearPixels.Num(); ++Index)
			{
				const FLinearColor& Scene = LinearPixels[Index];
				const FLinearColor Composited(
					Scene.R + Bg.R * Scene.A,
					Scene.G + Bg.G * Scene.A,
					Scene.B + Bg.B * Scene.A,
					1.0f);
				Pixels[Index] = Composited.ToFColor(/*bSRGB*/ true);
			}
		}
		else
		{
			FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
			ReadFlags.SetLinearToGamma(false);
			if (!Resource->ReadPixels(Pixels, ReadFlags))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("capture_failed"),
					TEXT("Failed to read pixels from the capture render target."));
			}
		}

		FString Base64;
		int32 Bytes = 0;
		FString Error;
		if (!EncodePngBase64(Pixels, Width, Height, Base64, Bytes, Error))
		{
			return FUnrealOpenMcpToolDispatchResult::Fail(
				Bytes == 0 ? TEXT("capture_failed") : TEXT("image_too_large"), Error);
		}

		return MakeImageResult(Source, Width, Height, Bytes, Base64);
	}

	/**
	 * Parse a '#RRGGBB' / '#RRGGBBAA' (bare 6- or 8-digit also accepted) hex
	 * color. FColor::FromHex silently returns black for malformed input, so
	 * validate explicitly and surface a structured error instead — this also
	 * gives the headless specs another GPU-free branch. Only the two lengths
	 * the tool advertises are accepted (no 3-digit shorthand).
	 */
	bool ParseHexColor(const FString& In, FLinearColor& OutColor, FString& OutError)
	{
		FString Hex = In;
		Hex.RemoveFromStart(TEXT("#"));
		if (Hex.Len() != 6 && Hex.Len() != 8)
		{
			OutError = FString::Printf(
				TEXT("Invalid 'background' hex color '%s'; expected '#RRGGBB' or '#RRGGBBAA'."), *In);
			return false;
		}
		for (const TCHAR Ch : Hex)
		{
			if (!FChar::IsHexDigit(Ch))
			{
				OutError = FString::Printf(
					TEXT("Invalid 'background' hex color '%s'; non-hex character found."), *In);
				return false;
			}
		}
		OutColor = FLinearColor(FColor::FromHex(Hex));
		return true;
	}
}

int32 FUnrealOpenMcpScreenshotTools::ResolveCaptureDimension(int64 Requested)
{
	if (Requested <= 0)
	{
		return DefaultCaptureDimension;
	}
	return static_cast<int32>(FMath::Clamp<int64>(Requested, static_cast<int64>(1), static_cast<int64>(MaxCaptureDimension)));
}

void FUnrealOpenMcpScreenshotTools::CapToMaxDimension(int32 InW, int32 InH, int32& OutW, int32& OutH)
{
	OutW = FMath::Max(InW, 1);
	OutH = FMath::Max(InH, 1);
	const int32 LongestSide = FMath::Max(OutW, OutH);
	if (LongestSide > MaxCaptureDimension)
	{
		const double Scale = static_cast<double>(MaxCaptureDimension) / static_cast<double>(LongestSide);
		OutW = FMath::Max(1, FMath::RoundToInt(OutW * Scale));
		OutH = FMath::Max(1, FMath::RoundToInt(OutH * Scale));
	}
}

bool FUnrealOpenMcpScreenshotTools::EncodePngBase64(
	TArray<FColor>& Pixels, int32 Width, int32 Height,
	FString& OutBase64, int32& OutEncodedBytes, FString& OutError)
{
	if (Width <= 0 || Height <= 0)
	{
		OutError = TEXT("Capture produced a zero-sized image.");
		return false;
	}
	if (Pixels.Num() < Width * Height)
	{
		OutError = FString::Printf(
			TEXT("Capture pixel buffer (%d) is smaller than %dx%d."),
			Pixels.Num(), Width, Height);
		return false;
	}
	// Force opaque in-place — the encode path owns this guarantee so the spec
	// can verify alpha→255 without reaching the capture helpers.
	for (FColor& Pixel : Pixels)
	{
		Pixel.A = 255;
	}

	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(
		Width, Height,
		TArrayView64<const FColor>(Pixels.GetData(), (int64)Width * (int64)Height),
		Png);
	if (Png.Num() == 0)
	{
		OutError = TEXT("PNG encoding produced no bytes.");
		return false;
	}
	if (Png.Num() > MaxEncodedBytes)
	{
		OutError = FString::Printf(
			TEXT("Encoded PNG (%lld bytes) exceeds the %lld-byte cap; request a smaller width/height."),
			(int64)Png.Num(), MaxEncodedBytes);
		return false;
	}
	OutBase64 = FBase64::Encode(Png.GetData(), Png.Num());
	OutEncodedBytes = (int32)Png.Num();
	return true;
}

void FUnrealOpenMcpScreenshotTools::Register(FUnrealOpenMcpToolRegistry& Registry)
{
	// =========================================================================
	// unreal_open_mcp_screenshot_viewport — active editor viewport.
	// =========================================================================
	//
	// `width` / `height` (optional, clamped to [1, 2048]; default native size;
	// lone axis derived from native aspect). Read-only (gate Off — reads
	// pixels, mutates no editor/project state). Result: an MCP image content
	// block + a `result` metadata object { source, width, height, mimeType,
	// byteSize }. Structured errors:
	//   - invalid_parameter      — malformed body
	//   - rendering_unavailable  — headless / -nullrhi
	//   - editor_unavailable     — no active editor viewport
	//   - capture_failed         — zero-sized viewport / read-back failed
	//   - image_too_large        — encoded PNG exceeds the byte cap
	Registry.Register(
		TEXT("unreal_open_mcp_screenshot_viewport"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			FString Error;
			if (!EnsureRenderingAvailable(Error))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("rendering_unavailable"), Error);
			}

			FViewport* Viewport = GEditor ? GEditor->GetActiveViewport() : nullptr;
			if (!Viewport)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("editor_unavailable"),
					TEXT("No active editor viewport. Focus a level editor viewport and retry."));
			}

			return CaptureFromViewport(Args, Viewport, TEXT("editor viewport"));
		});

	// =========================================================================
	// unreal_open_mcp_screenshot_game_view — PIE game view.
	// =========================================================================
	//
	// Same optional `width` / `height` as viewport. Read-only. The PIE
	// precondition is checked FIRST (GPU-free, headless-spec-covered): a
	// missing session returns `pie_not_running` before the GPU guard. Structured
	// errors:
	//   - invalid_parameter      — malformed body
	//   - pie_not_running        — no PIE session / no PIE viewport
	//   - rendering_unavailable  — headless / -nullrhi
	//   - capture_failed         — zero-sized viewport / read-back failed
	//   - image_too_large        — encoded PNG exceeds the byte cap
	Registry.Register(
		TEXT("unreal_open_mcp_screenshot_game_view"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			// Validate the PIE precondition FIRST (GPU-free).
			const bool bPlaySessionActive = GEditor && GEditor->PlayWorld != nullptr;
			FViewport* PieViewport = GEditor ? GEditor->GetPIEViewport() : nullptr;
			if (!bPlaySessionActive || !PieViewport)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("pie_not_running"),
					TEXT("No Play-In-Editor session is active. Start PIE before calling screenshot_game_view."));
			}

			FString Error;
			if (!EnsureRenderingAvailable(Error))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("rendering_unavailable"), Error);
			}

			return CaptureFromViewport(Args, PieViewport, TEXT("PIE game view"));
		});

	// =========================================================================
	// unreal_open_mcp_screenshot_camera — render from a camera actor.
	// =========================================================================
	//
	// `camera` (required, actor ref via P2 ResolveActor). `width` / `height`
	// (optional, default 1024 each). `fov` (optional, horizontal degrees,
	// clamped [5, 170]; defaults to the camera component's FOV or 90). Read-
	// only. Resolves the camera + validates args FIRST (GPU-free), then renders
	// via a transient SceneCapture2D at the camera's transform. Structured
	// errors:
	//   - invalid_parameter      — malformed body
	//   - missing_parameter      — `camera` absent
	//   - actor_not_found        — camera ref did not resolve
	//   - no_editor_world        — no editor world available
	//   - rendering_unavailable  — headless / -nullrhi
	//   - capture_failed         — spawn / capture / read-back failed
	//   - image_too_large        — encoded PNG exceeds the byte cap
	Registry.Register(
		TEXT("unreal_open_mcp_screenshot_camera"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString CameraRef = GetStringField(Args, TEXT("camera"));
			if (CameraRef.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'camera' is required (a camera actor reference: label, object name, or path)."));
			}

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (!World)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available."));
			}

			AActor* CameraActor = FUnrealOpenMcpObjectRef::ResolveActor(CameraRef, World);
			if (!CameraActor)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("actor_not_found"),
					FString::Printf(TEXT("No actor matched camera reference '%s'."), *CameraRef));
			}

			UCameraComponent* CameraComponent = CameraActor->FindComponentByClass<UCameraComponent>();
			// Clamp once at parse time; a degenerate FOV (<= 0 or >= 180)
			// yields a NaN/garbage projection matrix rather than a usable render.
			float Fov = 90.0f;
			if (Args->HasTypedField<EJson::Number>(TEXT("fov")))
			{
				Fov = GetNumberField(Args, TEXT("fov"));
			}
			else if (CameraComponent)
			{
				Fov = CameraComponent->FieldOfView;
			}
			Fov = FMath::Clamp(Fov, 5.0f, 170.0f);

			const FTransform CaptureXform = CameraComponent
				? CameraComponent->GetComponentTransform()
				: CameraActor->GetActorTransform();

			FString Error;
			if (!EnsureRenderingAvailable(Error))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("rendering_unavailable"), Error);
			}

			const int32 Width = ResolveCaptureDimension(GetIntField(Args, TEXT("width")));
			const int32 Height = ResolveCaptureDimension(GetIntField(Args, TEXT("height")));
			const FString Source = FString::Printf(TEXT("camera '%s'"), *CameraActor->GetActorNameOrLabel());
			return CaptureWithSceneCapture(World, CaptureXform, Fov, Width, Height, Source, nullptr, nullptr);
		});

	// =========================================================================
	// unreal_open_mcp_screenshot_isolated — isolated actor render.
	// =========================================================================
	//
	// `actor` (required). `width` / `height` (optional, default 1024 each).
	// `background` (optional hex '#RRGGBB' / '#RRGGBBAA'; default dark grey).
	// `fov` (optional, clamped [5, 170]; default 50). Read-only. Resolves the
	// actor + validates the hex FIRST (GPU-free), then renders via a transient
	// SceneCapture2D with a show-only list (target + recursively-attached
	// actors) composited over the background. Auto-framed by the actor's bounds.
	// Structured errors:
	//   - invalid_parameter      — malformed body / bad hex
	//   - missing_parameter      — `actor` absent
	//   - actor_not_found        — actor ref did not resolve
	//   - no_editor_world        — no editor world available
	//   - rendering_unavailable  — headless / -nullrhi
	//   - capture_failed         — spawn / capture / read-back failed
	//   - image_too_large        — encoded PNG exceeds the byte cap
	Registry.Register(
		TEXT("unreal_open_mcp_screenshot_isolated"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString ActorRef = GetStringField(Args, TEXT("actor"));
			if (ActorRef.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'actor' is required (the actor reference to render in isolation: label, object name, or path)."));
			}

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (!World)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available."));
			}

			AActor* TargetActor = FUnrealOpenMcpObjectRef::ResolveActor(ActorRef, World);
			if (!TargetActor)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("actor_not_found"),
					FString::Printf(TEXT("No actor matched reference '%s'."), *ActorRef));
			}

			// Parse + validate the background BEFORE the GPU guard so malformed
			// hex is a GPU-free error branch.
			FLinearColor Background(0.05f, 0.05f, 0.05f, 1.0f);
			FString Error;
			if (Args->HasTypedField<EJson::String>(TEXT("background")))
			{
				if (!ParseHexColor(Args->GetStringField(TEXT("background")), Background, Error))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("invalid_parameter"), Error);
				}
			}

			if (!EnsureRenderingAvailable(Error))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("rendering_unavailable"), Error);
			}

			// Auto-frame: place the capture along an isometric-ish offset so the
			// actor's bounding sphere fits the FOV. Clamp once here so the same
			// value drives both the auto-framing distance and the capture's
			// FOVAngle (an unclamped FOV would frame and render at different
			// angles).
			float Fov = 50.0f;
			if (Args->HasTypedField<EJson::Number>(TEXT("fov")))
			{
				Fov = GetNumberField(Args, TEXT("fov"));
			}
			Fov = FMath::Clamp(Fov, 5.0f, 170.0f);
			const int32 Width = ResolveCaptureDimension(GetIntField(Args, TEXT("width")));
			const int32 Height = ResolveCaptureDimension(GetIntField(Args, TEXT("height")));
			const FBox Bounds = TargetActor->GetComponentsBoundingBox(/*bNonColliding*/ true);
			const FVector Center = Bounds.IsValid ? Bounds.GetCenter() : TargetActor->GetActorLocation();
			const float Radius = Bounds.IsValid ? FMath::Max(Bounds.GetExtent().Size(), 1.0f) : 100.0f;
			const float HalfFovRad = FMath::DegreesToRadians(Fov * 0.5f);
			// FOVAngle is the HORIZONTAL field of view, so for a wider-than-tall
			// output the vertical FOV shrinks by Height/Width. Pull the camera
			// back by Max(1, Width/Height) so a wide aspect frames the bounding
			// sphere against the (smaller) vertical FOV instead of clipping the
			// actor off the top and bottom.
			const float AspectPullback = Height > 0 ? FMath::Max(1.0f, static_cast<float>(Width) / static_cast<float>(Height)) : 1.0f;
			const float Distance = (Radius / FMath::Max(FMath::Tan(HalfFovRad), KINDA_SMALL_NUMBER)) * 1.5f * AspectPullback;
			const FVector Offset = FVector(-1.0f, -1.0f, 0.6f).GetSafeNormal() * Distance;
			const FVector CamLocation = Center + Offset;
			const FRotator CamRotation = (Center - CamLocation).Rotation();
			const FTransform CaptureXform(CamRotation, CamLocation);

			const FString Source = FString::Printf(TEXT("isolated actor '%s'"), *TargetActor->GetActorNameOrLabel());
			return CaptureWithSceneCapture(World, CaptureXform, Fov, Width, Height, Source, &Background, TargetActor);
		});
}
