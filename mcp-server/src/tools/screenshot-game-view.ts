import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P5.5 — capture the PIE game view. Adapted from Unreal-MCP's screenshot-game-
// view. Read-only. The PIE precondition is checked before the GPU guard so a
// missing session returns pie_not_running deterministically (headless-testable).
//
// Soft dependency on editor_application_set_state (P5.1) for reliable play
// sessions: start PIE via set-state, poll get-state until isPlaying, then call
// this tool. Returns a base64 PNG as an MCP image content block.
//
// Route: live (POST /tools/unreal_open_mcp_screenshot_game_view). Read-only —
// no gate.
export const screenshotGameView: Tool = {
  name: "unreal_open_mcp_screenshot_game_view",
  description:
    "Capture the Play-In-Editor (PIE) game view and return it as a base64 PNG " +
    "(MCP image content block). Read-only (gate-free). Errors with " +
    "pie_not_running when no PIE session is active — start PIE via " +
    "editor_application_set_state first and poll editor_application_get_state " +
    "until isPlaying is true. Optional 'width'/'height' are clamped to " +
    "[1, 2048] per side; when only one is given the other is derived from the " +
    "native game-view aspect ratio, and the native game-view size is used when " +
    "both are omitted. The PNG is always opaque. Requires a GPU-backed " +
    "(windowed) editor. Returns an image content block + a text metadata " +
    "block { source, width, height, mimeType, byteSize }. Error codes: " +
    "invalid_parameter (malformed body), pie_not_running (no PIE session), " +
    "rendering_unavailable (headless/-nullrhi), capture_failed (zero-sized " +
    "viewport / read-back failed), image_too_large (encoded PNG exceeds the " +
    "byte cap).",
  inputSchema: {
    type: "object",
    properties: {
      width: {
        type: "integer",
        description:
          "Optional output width in pixels (clamped to [1, 2048]); when only " +
          "'height' is given the width is derived from the native game-view " +
          "aspect ratio.",
      },
      height: {
        type: "integer",
        description:
          "Optional output height in pixels (clamped to [1, 2048]); when only " +
          "'width' is given the height is derived from the native game-view " +
          "aspect ratio.",
      },
    },
    additionalProperties: false,
  },
};
