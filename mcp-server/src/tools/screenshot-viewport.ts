import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P5.5 — capture the active editor viewport. One of the four-tool screenshot
// family (viewport / game-view / camera / isolated) adapted from Unreal-MCP's
// screenshot-viewport (FViewport::ReadPixels). Read-only.
//
// Returns a base64 PNG as an MCP image content block (the bridge carries the
// image in a top-level `image` field; the LiveClient unwraps it into an image
// block so the base64 never surfaces as text). The accompanying text block
// carries { source, width, height, mimeType, byteSize } metadata.
//
// Route: live (POST /tools/unreal_open_mcp_screenshot_viewport). Read-only —
// no gate (reads pixels, mutates no editor/project state). Requires a GPU-
// backed (windowed) editor.
export const screenshotViewport: Tool = {
  name: "unreal_open_mcp_screenshot_viewport",
  description:
    "Capture the active editor viewport and return it as a base64 PNG (MCP " +
    "image content block). Read-only (gate-free). Optional 'width'/'height' " +
    "are clamped to [1, 2048] per side; when only one is given the other is " +
    "derived from the native viewport aspect ratio, and the native viewport " +
    "size is used when both are omitted. The PNG is always opaque (alpha " +
    "forced to 255). Note: while a Play-In-Editor session has viewport focus " +
    "the 'active viewport' is the PIE game view — use screenshot_game_view to " +
    "capture the game view explicitly. Requires a GPU-backed (windowed) " +
    "editor. Returns an image content block + a text metadata block " +
    "{ source, width, height, mimeType, byteSize }. Error codes: " +
    "invalid_parameter (malformed body), rendering_unavailable (headless/" +
    "-nullrhi), editor_unavailable (no active viewport), capture_failed " +
    "(zero-sized viewport / read-back failed), image_too_large (encoded PNG " +
    "exceeds the byte cap — request a smaller width/height).",
  inputSchema: {
    type: "object",
    properties: {
      width: {
        type: "integer",
        description:
          "Optional output width in pixels (clamped to [1, 2048]); when only " +
          "'height' is given the width is derived from the native aspect " +
          "ratio, and the native viewport width is used when both are omitted.",
      },
      height: {
        type: "integer",
        description:
          "Optional output height in pixels (clamped to [1, 2048]); when only " +
          "'width' is given the height is derived from the native aspect " +
          "ratio, and the native viewport height is used when both are omitted.",
      },
    },
    additionalProperties: false,
  },
};
