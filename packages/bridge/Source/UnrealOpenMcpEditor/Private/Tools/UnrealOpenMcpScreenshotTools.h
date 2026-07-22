// Screenshot tool family for the bridge tool surface (P5.5).
//
// Four read-only image-capture tools that return a base64 PNG as MCP image
// content (via the FUnrealOpenMcpToolDispatchResult image payload → bridge
// envelope top-level `image` field → MCP LiveClient image-content-block unwrap):
//
//   - `unreal_open_mcp_screenshot_viewport`  — active editor viewport via
//     FViewport::ReadPixels.
//   - `unreal_open_mcp_screenshot_game_view` — PIE viewport; structured error
//     (`pie_not_running`) when no PIE session is active.
//   - `unreal_open_mcp_screenshot_camera`    — render from a resolved camera
//     actor through a transient SceneCapture2D into a transient render target.
//   - `unreal_open_mcp_screenshot_isolated`  — isolated actor render: transient
//     SceneCapture2D + show-only list + neutral background, auto-framed by the
//     actor's bounds.
//
// Dimension caps (shared): default 1024, hard cap 2048 per side. Encoded byte
// cap 40 MiB (base64 expands ~4/3, so the payload stays well under the IPC
// line limit). Pixels are forced opaque so a viewport/render-target alpha of 0
// never yields a transparent PNG (transparent PNGs are useless to agents).
//
// Every handler validates its arguments FIRST (those branches are GPU-free and
// headless-spec-covered), and only then attempts the GPU read-back — returning
// a structured `rendering_unavailable` error under `-nullrhi` instead of
// crashing. The capture paths themselves are live-verified windowed (manual GPU
// checklist); the Automation spec pins the GPU-free branches (dimension clamp,
// opaque-force, PNG signature, byte-cap refuse, arg validation).
//
// Adapted (read-only behavior reference) from Unreal-MCP's screenshot handlers
// (UnrealMcpScreenshotTools.cpp — screenshot-viewport / -game-view / -camera /
// -isolated): the FViewport ReadPixels + transient SceneCapture2D + show-only
// list + HDR-over-background composite logic. The MCP image transport is
// copied from Unity Open MCP's inlineImage unwrap pattern. Tool names follow
// the project's `unreal_open_mcp_*` convention (ADR-003).
//
// Every handler runs ON THE GAME THREAD (the HTTP server marshals dispatch
// through the GameThreadDispatcher), so the RHI / viewport / UObject APIs are
// touched safely. Transient capture actors + render targets are cleaned up on
// every return path (ON_SCOPE_EXIT destroy + FGCObjectScopeGuard) so the editor
// world is never dirtied.
#pragma once

#include "CoreMinimal.h"

class FUnrealOpenMcpToolRegistry;

/**
 * Register the screenshot tool family with @p Registry. Registers four
 * read-only tools (gate Off — they read pixels but mutate no editor/project
 * state):
 *   `unreal_open_mcp_screenshot_viewport`,
 *   `unreal_open_mcp_screenshot_game_view`,
 *   `unreal_open_mcp_screenshot_camera`,
 *   `unreal_open_mcp_screenshot_isolated`.
 * First-registration-wins: a duplicate name is ignored by the registry.
 */
namespace FUnrealOpenMcpScreenshotTools
{
	void Register(FUnrealOpenMcpToolRegistry& Registry);

	/** Hard cap on each output dimension (width / height), in pixels. */
	static constexpr int32 MaxCaptureDimension = 2048;
	/** Default output dimension when the caller omits width/height. */
	static constexpr int32 DefaultCaptureDimension = 1024;
	/** Hard cap on the encoded PNG byte count. Keeps the base64 payload (~4/3
	 *  expansion) well under the bridge IPC line ceiling. A capture exceeding
	 *  this is refused with image_too_large. */
	static constexpr int64 MaxEncodedBytes = 40 * 1024 * 1024;

	/**
	 * Resolve a requested dimension to a valid pixel count. `<= 0` (omitted)
	 * → DefaultCaptureDimension (1024); otherwise clamped to [1,
	 * MaxCaptureDimension]. Exported so the headless Automation spec can pin
	 * the clamp without a GPU.
	 */
	UNREALOPENMCPEDITOR_API int32 ResolveCaptureDimension(int64 Requested);

	/**
	 * Proportionally downscale (InW, InH) so the longest side is at most
	 * MaxCaptureDimension. No-op when already within the cap; floors each side
	 * at 1. Exported for the headless spec.
	 */
	UNREALOPENMCPEDITOR_API void CapToMaxDimension(int32 InW, int32 InH, int32& OutW, int32& OutH);

	/**
	 * Encode a BGRA8 FColor buffer to a base64 PNG, forcing every pixel opaque
	 * first (alpha → 255) so a viewport/render-target alpha of 0 never yields a
	 * transparent PNG. Guards: zero-size image, under-size buffer, empty PNG
	 * output, and the MaxEncodedBytes byte cap. On success @p OutBase64 carries
	 * the raw base64 (no `data:` URI prefix) and @p OutEncodedBytes the raw PNG
	 * byte count; on failure @p OutError carries the structured message.
	 *
	 * Exported so the headless Automation spec can pin the PNG signature, the
	 * opaque-force behavior, and the byte-cap refusal WITHOUT a GPU (the encode
	 * path is pure CPU — FImageUtils::PNGCompressImageArray over an in-memory
	 * FColor buffer).
	 */
	UNREALOPENMCPEDITOR_API bool EncodePngBase64(
		TArray<FColor>& Pixels, int32 Width, int32 Height,
		FString& OutBase64, int32& OutEncodedBytes, FString& OutError);
}
