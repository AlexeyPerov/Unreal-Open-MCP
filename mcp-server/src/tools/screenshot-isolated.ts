import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P5.5 — render a single actor in isolation against a neutral background,
// auto-framed by its bounds. Adapted from Unreal-MCP's screenshot-isolated
// (transient SceneCapture2D + show-only list + HDR-over-background composite).
// Read-only.
//
// The show-only list includes the target actor AND its recursively-attached
// children (child-actor components, attach hierarchies) so visually-integral
// parts of composed Blueprints are not silently omitted. The background is
// composited in linear space (scene-over-background) because a plain LDR
// capture would overwrite the clear color with the scene's black background.
//
// Route: live (POST /tools/unreal_open_mcp_screenshot_isolated). Read-only —
// no gate.
export const screenshotIsolated: Tool = {
  name: "unreal_open_mcp_screenshot_isolated",
  description:
    "Render a single actor in isolation against a neutral background, auto-" +
    "framed by its bounds, and capture it as a base64 PNG (MCP image content " +
    "block). Read-only (gate-free). 'actor' resolves via the actor-ref " +
    "resolver (label → object name → path). The show-only list includes the " +
    "target plus its recursively-attached children so composed Blueprints " +
    "render completely. Optional 'width'/'height' default to 1024 each " +
    "(clamped to [1, 2048]). Optional 'background' is a hex color " +
    "('#RRGGBB' or '#RRGGBBAA'; default dark grey #0D0D0D). Optional 'fov' " +
    "defaults to 50 (clamped to [5, 170]). The PNG is always opaque. Transient " +
    "capture actor + render target are cleaned up on every return path. " +
    "Requires a GPU-backed (windowed) editor. Returns an image content block + " +
    "a text metadata block { source, width, height, mimeType, byteSize }. " +
    "Error codes: invalid_parameter (malformed body / bad hex), " +
    "missing_parameter (actor absent), actor_not_found (actor ref did not " +
    "resolve), no_editor_world, rendering_unavailable (headless/-nullrhi), " +
    "capture_failed (spawn / capture / read-back failed), image_too_large " +
    "(encoded PNG exceeds the byte cap).",
  inputSchema: {
    type: "object",
    required: ["actor"],
    properties: {
      actor: {
        type: "string",
        description:
          "Target actor reference to render in isolation (actor label, object " +
          "name, or path). Resolved via the same label → name → path resolver " +
          "as the actor family.",
      },
      width: {
        type: "integer",
        description: "Optional output width in pixels (default 1024, clamped to [1, 2048]).",
      },
      height: {
        type: "integer",
        description: "Optional output height in pixels (default 1024, clamped to [1, 2048]).",
      },
      background: {
        type: "string",
        description:
          "Optional background color as hex ('#RRGGBB' or '#RRGGBBAA'); the " +
          "alpha of '#RRGGBBAA' is accepted but ignored (the PNG is always " +
          "opaque). Defaults to dark grey (#0D0D0D).",
      },
      fov: {
        type: "number",
        description: "Optional field-of-view override in degrees (default 50, clamped to [5, 170]).",
      },
    },
    additionalProperties: false,
  },
};
