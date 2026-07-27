import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P7.3 — source_compile: the AI feedback loop for C++. Compiles the project's
// C++ and returns a STRUCTURED diagnostic report — `{ file, line, severity,
// message }[]` parsed out of UBT's MSVC + clang stdout/stderr by the bridge's
// `ParseDiagnostics` (exported from `UnrealOpenMcpSourceTools.h` so the
// Automation spec drives it with canned fixtures — no UBT invocation).
//
// Two paths:
//   1. Live Coding — preferred when the editor is interactive + LC is live +
//      `use_live_coding` is true. Patches the running module DLL in place (no
//      relink). Returns a COARSE result enum (`Success` / `NoChanges` /
//      `Failure` / ...) with NO per-diagnostic rows — LC does not surface a
//      structured report. On `Failure` or `NotStarted`, the bridge falls
//      through to UBT so the agent still gets a structured report. Only
//      available on Windows hosts where the `LiveCoding` engine module is
//      compiled in (gated by `WITH_UNREAL_MCP_LIVE_CODING`); other hosts +
//      headless / `-game` runs take the UBT path directly.
//   2. UBT — the fallback + forced path (`use_live_coding:false`). Resolves
//      `UnrealBuildTool.exe` / `RunUBT.sh`, validates `target` / `platform` /
//      `configuration` as identifier-only tokens (no arg injection — a value
//      like `"MyGameEditor Win64 Development -Clean"` would otherwise wipe
//      Binaries/Intermediate), runs `ExecProcess` with
//      `-project=<uproject> -WaitMutex`, and parses the combined output.
//
// `success` (process return code 0) is reported SEPARATELY from
// `compile_clean` (zero compiler-error diagnostics). A loaded editor holds its
// module DLL, so a plain UBT relink fails to write it (`success:false`) — but
// compiler errors are emitted BEFORE the link stage, so a clean compile is
// `compile_clean:true` even when `success:false`. The AI loop keys off
// `compile_clean` + the `diagnostics[]`, NOT off `success`.
//
// A FAILED compile is a NORMAL, expected result, NOT a transport failure. The
// envelope stays `ok:true` and the result carries `success:false` +
// `compile_clean:false` + a populated `diagnostics[]` so an agent reads the
// rows, fixes via `source_update` / `source_create_class`, and recompiles.
// Only TOOL-LEVEL errors (UBT binary missing, invalid identifier token, UBT
// launch failure, malformed body) map to `ok:false`. This mirrors P6.5
// `blueprint_compile`'s failed-compile-as-data contract — the same intent
// ("compile failed" is data, not an opaque `isError`).
//
// Sync (MVP): UBT runs to completion on the game thread inside the handler.
// Document the timeout — a from-scratch Editor build can take minutes; raise
// the MCP client timeout for this tool in tight Automation loops, or prefer
// `use_live_coding:true` (interactive) for sub-second patches. An async job
// queue is backlog if timeouts become chronic.
//
// Mutating: a compile rebuilds module DLLs + dirties the project, so it runs
// the full gate path (checkpoint -> compile -> validate -> delta); `paths_hint`
// MUST list the touched module folder or files under `Source/` (e.g.
// `['MyGame/']`) — there is no whole-project fallback, set `gate:"off"` to
// bypass. The gate's post-mutation validate pass may surface verify
// `compile_errors` (P3.4) under `enforce` — desirable (same source, same
// diagnostics) — pass `gate:"warn"` or `gate:"off"` for a tight recompile loop
// that always returns the `result`.
//
// Fidelity: greenfield. No Unity UBT / Live Coding twin (loose workflow analogy
// only: Unity's `compile_check` / `read_compile_errors` — different engine, no
// shared code, do not force schema parity). Behavior reference (read-only):
// Unreal-MCP's `source-compile` for the Live Coding `#if` + UBT `ExecProcess` +
// `ParseDiagnostics` shape, mapped to the Open MCP `{ok, result}` envelope.
//
// Intentional deltas vs Unreal-MCP:
//   - Open MCP `{ok, result}` mapping — `isError` stays false for ordinary
//     compile diagnostics (the failed-compile-as-data contract).
//   - A gate summary may accompany the compile under `enforce`.
//   - snake_case field names (`use_live_coding`, `compile_clean`,
//     `error_count`, `return_code`, `duration_seconds`, `output_tail`).
//
// Route: live (POST /tools/unreal_open_mcp_source_compile). Mutating.
export const sourceCompile: Tool = {
  name: "unreal_open_mcp_source_compile",
  description:
    "Compile the project's C++ and return a STRUCTURED diagnostic report — " +
    "the AI feedback loop. Prefers Live Coding when the editor is interactive " +
    "+ Live Coding is live + use_live_coding is true (Windows only; patches " +
    "the running module DLL in place — no relink), otherwise invokes " +
    "UnrealBuildTool on the project's Editor target (target overrides; " +
    "default '<Project>Editor'). Set use_live_coding:false to force the UBT " +
    "path. A FAILED compile is a NORMAL, expected result, NOT a transport " +
    "failure: the envelope stays ok:true and the result carries " +
    "success:false + compile_clean:false + a populated diagnostics[] so you " +
    "can read the rows, fix via source_update / source_create_class, and " +
    "recompile. Only tool-level errors (UBT binary missing, invalid " +
    "identifier token, UBT launch failure, malformed body) map to ok:false. " +
    "This is the spine of the C++ edit loop — source_create_class / " +
    "source_update only land once source_compile runs. success (return_code " +
    "== 0) is SPLIT from compile_clean (zero compiler errors): a loaded " +
    "editor holds its module DLL, so a UBT relink fails to write it " +
    "(success:false) even when the compile stage was clean " +
    "(compile_clean:true) — key off compile_clean + diagnostics, NOT success. " +
    "The expected loop is: edit -> compile -> (read diagnostics -> fix -> " +
    "recompile until compile_clean). Mutating: a compile rebuilds module " +
    "DLLs + dirties the project, so it runs the full gate path (checkpoint " +
    "-> compile -> validate -> delta); paths_hint MUST list the touched " +
    "module folder or files under Source/ (e.g. ['MyGame/']) — there is no " +
    "whole-project fallback, set gate:\"off\" to bypass. Result shape (UBT): " +
    "{ method:'ubt', target, configuration, platform, return_code, success, " +
    "compile_clean, error_count, warning_count, duration_seconds, " +
    "diagnostics:[{file,line,severity,message}], output_tail }. Result shape " +
    "(Live Coding): { method:'live_coding', result ('Success'|'NoChanges'| " +
    "'Failure'|...), success, compile_clean, error_count, warning_count, " +
    "diagnostics:[] } (LC surfaces a coarse enum, not per-diagnostic rows — " +
    "on Failure the bridge falls through to UBT for the full report). Error " +
    "codes: invalid_parameter (malformed body OR a non-identifier " +
    "target/platform/configuration — no arg injection), ubt_not_found (no " +
    "UnrealBuildTool binary at the resolved engine path), ubt_launch_failed " +
    "(ExecProcess could not start UBT). NOTE: a non-zero return_code / a " +
    "populated diagnostics[] is NOT one of these codes — it rides through as " +
    "success:false / compile_clean:false on an ok:true envelope. SYNC (MVP): " +
    "UBT runs to completion inside the handler; raise the MCP client timeout " +
    "for this tool or prefer use_live_coding:true (interactive) for " +
    "sub-second patches.",
  inputSchema: {
    type: "object",
    required: ["paths_hint"],
    properties: {
      target: {
        type: "string",
        description:
          "Build target name. Default '<Project>Editor'. Must be a bare " +
          "identifier (letters / digits / underscore, no leading digit) — no " +
          "whitespace, quotes, or dash-prefixed flags (else invalid_parameter; " +
          "prevents UBT arg injection, e.g. a '-Clean' suffix would wipe " +
          "Binaries/Intermediate).",
      },
      configuration: {
        type: "string",
        default: "Development",
        description:
          "Build configuration. Default 'Development'. Identifier-only " +
          "(same anti-injection rule as target).",
      },
      platform: {
        type: "string",
        description:
          "Build platform. Default the host platform's binaries subdirectory " +
          "(e.g. 'Win64' / 'Mac' / 'Linux'). Identifier-only (same " +
          "anti-injection rule as target).",
      },
      use_live_coding: {
        type: "boolean",
        default: true,
        description:
          "Prefer Live Coding when available (interactive editor + LC session " +
          "live + Windows host with the LiveCoding module compiled in). " +
          "Default true. Set false to force the UBT path (the only path on " +
          "non-Windows hosts and in headless / -game runs). On a Live Coding " +
          "Failure the bridge falls through to UBT for the full structured " +
          "report.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — the touched module folder or source file(s) the " +
          "compile is scoped to, fed to the gate as the checkpoint + validate " +
          "hint. e.g. ['MyGame/'] or ['MyGame/MyActor.cpp']. REQUIRED for " +
          "mutating tools (the gate refuses an empty hint with " +
          "paths_hint_required; there is no whole-project fallback). Set " +
          "gate:\"off\" to bypass the gate and skip the hint.",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint -> compile -> " +
          "validate -> delta and hard-fails on new Errors surfaced by the " +
          "verify rules (e.g. P3.4 compile_errors — desirable, same source " +
          "same diagnostics); warn commits the compile but surfaces new " +
          "Errors as warnings; off skips the gate entirely (paths_hint " +
          "optional). For a tight recompile loop that always returns the " +
          "result, pass gate:\"warn\" or gate:\"off\".",
      },
    },
    additionalProperties: false,
  },
};
