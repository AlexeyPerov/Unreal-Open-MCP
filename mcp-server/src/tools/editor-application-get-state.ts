import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P5.1 — read-only editor application (PIE) state snapshot. The Unreal analog
// of Unity's editor-status, adapted to the Unreal Play-In-Editor field set.
//
// Reports whether a Play-In-Editor / Simulate-In-Editor session is running and
// which map the editor is editing, so an agent can gate play-mode workflows
// (game-view screenshots, runtime actor inspection) on a truthful play state.
//
// Intentional deltas vs Unity's editor-status:
//   - The play model is an Unreal PIE field set (isPlaying / isPaused /
//     isSimulating) rather than Unity's single is_playing boolean; isSimulating
//     is the Unreal-specific Simulate-In-Editor flag with no direct Unity twin.
//   - `editorMap` is the editor world's persistent-level package path
//     ('/Game/Maps/Arena', '/Temp/Untitled' for a fresh map); `editorMapName`
//     is the short map name. Always the editing world, never the transient PIE
//     world.
//
// Route: live (POST /tools/unreal_open_mcp_editor_application_get_state).
// Read-only — no gate.
export const editorApplicationGetState: Tool = {
  name: "unreal_open_mcp_editor_application_get_state",
  description:
    "Read the editor's Play-In-Editor (PIE) state. Read-only (gate-free). " +
    "Returns { isPlaying, isPaused, isSimulating, editorMap, editorMapName }: " +
    "isPlaying is true when a real PIE session is running; isSimulating is true " +
    "under Simulate-In-Editor; isPaused is true when the PIE world is paused; " +
    "editorMap is the editor world's persistent-level package path " +
    "('/Game/Maps/Arena', '/Temp/Untitled' for an unsaved map) and editorMapName " +
    "is its short name (always the editing world, never the transient PIE " +
    "world). Poll this after editor_application_set_state start/stop to observe " +
    "the settled (latent) transition — set-state returns pending:true and the " +
    "PIE world flips on a later editor tick. Error code: editor_unavailable " +
    "(no editor / no editor world, e.g. a commandlet).",
  inputSchema: {
    type: "object",
    properties: {},
    additionalProperties: false,
  },
};
