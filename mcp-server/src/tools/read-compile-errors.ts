import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P8.7 — offline read_compile_errors: read C++ / UBT / Live Coding diagnostics
// from the Unreal editor log tail on disk, with the bridge DOWN. The one
// recovery channel that works when the bridge module itself failed to compile
// (Live Coding failure / a bad C++ edit) — in that state every in-bridge channel
// (console_get_logs, source_compile, the ping probe) is dead with it, and the
// bridge assembly cannot load to serve any handler. The live Editor still writes
// UBT / MSVC / clang diagnostics to `<Project>/Saved/Logs/*.log` regardless of
// bridge health, so this tool retrieves them without touching the bridge.
//
// Mirrors the live `source_compile` diagnostic shape
// (`{ file, line, severity, message }[]`) via the offline twin of the bridge's
// `ParseDiagnostics` — same MSVC + clang regexes, so an agent fixes a diagnostic
// the same way whether it came from a live compile or a dead-bridge log tail.
//
// Adapted from Unity Open MCP's mcp-server/src/tools/read-compile-errors.ts
// (copy fidelity for the offline/ADR-scope spirit + tail_bytes arg). Intentional
// deltas: Unity reads a global Editor.log + project-relative 6000.5+ override;
// Unreal reads the newest `.log` under `<Project>/Saved/Logs/` (per-project
// logs, no global fallback). Unity surfaces package/assembly red flags; Unreal's
// equivalent red flags ARE compiler errors (UBT/MSVC/clang) and surface via the
// same parser.
//
// Route: **offline** (always — resolved from disk; never hits the bridge). The
// router stamps `_source: "offline"` + `_route: { route: "offline" }`.
export const readCompileErrors: Tool = {
  name: "unreal_open_mcp_read_compile_errors",
  description:
    "Read C++ / UnrealBuildTool / Live Coding compiler errors directly from the " +
    "Unreal editor log tail on disk (offline — no bridge, no editor spawn). " +
    "Returns structured MSVC / clang diagnostics `{ file, line, severity, " +
    "message }[]` parsed out of the newest `.log` under " +
    "<Project>/Saved/Logs/, the SAME shape the live source_compile tool returns " +
    "(an agent fixes a diagnostic the same way regardless of source). Use this " +
    "when: (a) the bridge is unreachable after a recompile — a dead_bridge " +
    "status points here; (b) bridge_status returns dead_bridge and ping fails " +
    "unexpectedly; (c) the editor appears hung after a C++ edit with the bridge " +
    "module unable to load. Works even when the bridge assembly itself is " +
    "broken, because it reads the log file the editor writes independently of " +
    "the bridge. Resolution: newest `.log` under <Project>/Saved/Logs/ by mtime " +
    "(Unreal rotates logs as <Name>.log + timestamped backups); there is NO " +
    "global per-user fallback — when no project log exists the tool reports " +
    "`log_not_found` honestly. Check `unhealthy` first; when true, scan " +
    "`headline` for a one-line triage then drill into `errors`. SCOPE (ADR-006): " +
    "this is a project-files / logs / source-text offline read — it does NOT " +
    "parse `.uasset` assets offline. Result shape: { status ('compile_failed' | " +
    "'no_errors_found' | 'log_not_found'), unhealthy, headline, error_count, " +
    "errors:[{file,line,severity,message}], logPath, tailBytes }. Route: " +
    "offline (always). Error codes: editor_log_unreadable (the log existed but " +
    "could not be read). A missing log is NOT an error — it returns " +
    "status:'log_not_found' on a non-error envelope.",
  inputSchema: {
    type: "object",
    properties: {
      tail_bytes: {
        type: "integer",
        default: 262144,
        minimum: 4096,
        maximum: 1048576,
        description:
          "Maximum bytes to read from the END of the log (default 256KB, clamped " +
          "to [4096, 1048576]). Compiler diagnostics are written in contiguous " +
          "blocks near the end of the log, so a modest tail is ample. Increase " +
          "only if errors are reported missing.",
      },
    },
    additionalProperties: false,
  },
};
