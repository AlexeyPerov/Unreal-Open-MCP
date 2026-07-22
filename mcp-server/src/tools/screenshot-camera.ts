import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P5.5 — render the scene from a chosen camera actor. Adapted from Unreal-
// MCP's screenshot-camera (transient SceneCapture2D into a render target).
// Read-only. Resolves the camera via the P2 actor-ref resolver (label → name
// → path), then renders at the camera's transform/FOV through a transient
// SceneCapture2D cleaned up on every path.
//
// Route: live (POST /tools/unreal_open_mcp_screenshot_camera). Read-only —
// no gate.
export const screenshotCamera: Tool = {
  name: "unreal_open_mcp_screenshot_camera",
  description:
    "Render the scene from a chosen camera actor and capture it as a base64 " +
    "PNG (MCP image content block). Read-only (gate-free). 'camera' resolves " +
    "via the actor-ref resolver (label → object name → path, case-sensitive " +
    "first then case-insensitive). Optional 'width'/'height' default to 1024 " +
    "each (clamped to [1, 2048]). Optional 'fov' overrides the horizontal " +
    "field-of-view in degrees (clamped to [5, 170]); defaults to the camera " +
    "component's FOV (or 90 if the actor has no CameraComponent — the tool " +
    "still works on a non-camera actor, rendering from its transform). The " +
    "PNG is always opaque. Transient capture actor + render target are cleaned " +
    "up on every return path. Requires a GPU-backed (windowed) editor. " +
    "Returns an image content block + a text metadata block { source, width, " +
    "height, mimeType, byteSize }. Error codes: invalid_parameter (malformed " +
    "body), missing_parameter (camera absent), actor_not_found (camera ref did " +
    "not resolve), no_editor_world, rendering_unavailable (headless/-nullrhi), " +
    "capture_failed (spawn / capture / read-back failed), image_too_large " +
    "(encoded PNG exceeds the byte cap).",
  inputSchema: {
    type: "object",
    required: ["camera"],
    properties: {
      camera: {
        type: "string",
        description:
          "Camera actor reference to render from (actor label, object name, " +
          "or path). Resolved via the same label → name → path resolver as " +
          "the actor family.",
      },
      width: {
        type: "integer",
        description: "Optional output width in pixels (default 1024, clamped to [1, 2048]).",
      },
      height: {
        type: "integer",
        description: "Optional output height in pixels (default 1024, clamped to [1, 2048]).",
      },
      fov: {
        type: "number",
        description:
          "Optional horizontal field-of-view override in degrees (clamped to " +
          "[5, 170]); defaults to the camera component's FOV (or 90).",
      },
    },
    additionalProperties: false,
  },
};
