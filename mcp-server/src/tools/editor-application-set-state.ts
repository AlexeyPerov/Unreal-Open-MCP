import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P5.1 — drive Play-In-Editor transitions. The Unreal analog of Unity's
// editor-set-state, adapted to Unreal's request-then-tick PIE lifecycle.
//
// `action` is an enum (start / stop / pause / resume) rather than Unity's
// is_playing boolean, because the Unreal PIE lifecycle is latent: every
// transition is applied on a later editor tick. start/stop queue the
// RequestPlaySession / RequestEndPlayMap; pause/resume toggle
// GEditor->SetPIEWorldsPaused (the editor pause flag bDebugPauseExecution).
// So ALL FOUR return { action, pending:true } and the agent polls
// editor_application_get_state to observe the settled state (the Unreal-MCP
// honesty pattern — never claim an unobserved transition).
//
// Invalid transitions return a structured invalid_transition (no silent
// restart): start while a session is already active, stop/pause/resume while no
// session is active, pause while already paused, resume while not paused.
//
// Gate: mutating (drives the editor process). `paths_hint` is REQUIRED — the
// gate refuses an empty hint with paths_hint_required (no whole-project
// fallback); pass the current map package and/or a project scope. Set
// gate:"off" to bypass. Route: live
// (POST /tools/unreal_open_mcp_editor_application_set_state).
export const editorApplicationSetState: Tool = {
  name: "unreal_open_mcp_editor_application_set_state",
  description:
    "Drive Play-In-Editor (PIE) transitions by action: start | stop | pause | " +
    "resume. ALL FOUR transitions are LATENT — they are applied on a later " +
    "editor tick (start/stop via RequestPlaySession/RequestEndPlayMap; " +
    "pause/resume via GEditor->SetPIEWorldsPaused) and return " +
    "{ action, pending:true }; poll editor_application_get_state until " +
    "isPlaying/isPaused flip. Invalid transitions return invalid_transition " +
    "(no silent restart): start while already playing, stop/pause/resume while " +
    "not playing, pause while already paused, resume while not paused. " +
    "Mutating: `paths_hint` is required (the gate refuses an empty hint with " +
    "paths_hint_required — pass the current map package and/or project scope; " +
    'set gate:"off" to bypass). Error codes: missing_parameter (action absent), ' +
    "invalid_parameter (action not a string or not one of " +
    "start|stop|pause|resume), invalid_transition, editor_unavailable (no " +
    "editor). v1 drives the in-process PlayInEditor session only (no " +
    "separate-process / multiplayer PIE variants).",
  inputSchema: {
    type: "object",
    required: ["action"],
    properties: {
      action: {
        enum: ["start", "stop", "pause", "resume"],
        description:
          "The PIE transition to request. All four are latent (pending:true, " +
          "poll get-state): start/stop queue the session transition; " +
          "pause/resume toggle the editor pause flag.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — the current map package path and/or project scope, " +
          "fed to the gate as the checkpoint + validate hint. REQUIRED for this " +
          "mutating tool (the gate refuses an empty hint with " +
          "paths_hint_required; there is no whole-project fallback). Set " +
          'gate:"off" to bypass the gate and skip the hint.',
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint → mutate → validate → " +
          "delta and hard-fails on new Errors; warn commits but surfaces new " +
          "Errors as warnings; off skips the gate entirely (paths_hint " +
          "optional). Precedence: request gate → tool default (enforce).",
      },
    },
    additionalProperties: false,
  },
};
