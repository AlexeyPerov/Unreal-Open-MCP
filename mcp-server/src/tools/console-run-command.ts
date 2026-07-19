import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P5.3 — run a console command via GEngine->Exec against the editor world. The
// Unreal-MCP console-run-command behavior (no direct Unity twin).
//
// This is a DESTRUCTIVE surface — a console command can change project /
// editor / render state (or worse). It is the same accepted-risk class as
// reflection_method_call: v1 has no per-command allow-list; the mandatory gate
// (paths_hint) is the safeguard, and this description is the warning.
//
// Route: live (POST /tools/unreal_open_mcp_console_run_command). Mutating.
export const consoleRunCommand: Tool = {
  name: "unreal_open_mcp_console_run_command",
  description:
    "Run a console command (GEngine->Exec) against the editor world and return " +
    "{ command, output, handled } where `output` is the command's captured text " +
    "and `handled` is true when a command handler consumed it. WARNING: this is " +
    "a destructive surface — a console command can change project/editor/render " +
    "state; it is the same accepted-risk class as reflection_method_call (no " +
    "per-command allow-list in v1). Mutating: `paths_hint` is required (the gate " +
    "refuses an empty hint with paths_hint_required — pass the project/map " +
    "scope; set gate:\"off\" to bypass). Error codes: missing_parameter " +
    "(command absent/empty), invalid_parameter (malformed body), " +
    "editor_unavailable (no engine / no editor world). Prefer purpose-built " +
    "tools (actor/level/material/asset families) over raw console commands when " +
    "one exists.",
  inputSchema: {
    type: "object",
    required: ["command"],
    properties: {
      command: {
        type: "string",
        description:
          "The console command line to execute, e.g. 'stat fps', " +
          "'r.ScreenPercentage 50', 'ke * MyEvent'.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — the project/map path(s) the command is scoped to, " +
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
