import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// Blueprint compile. Compiles a Blueprint via
// FKismetEditorUtilities::CompileBlueprint with a silent FCompilerResultsLog
// and returns a STRUCTURED error/warning list — the AI feedback loop. `path`
// is the Blueprint asset object path (package-path form also accepted —
// resolved in-memory first, then loaded).
//
// A FAILED compile is a NORMAL, expected result, NOT a transport failure. The
// envelope stays `ok: true` and the result object carries `succeeded: false`
// plus a populated `messages[]` so an agent reads the diagnostics, fixes the
// structure via the add/modify tools, and recompiles. Only TOOL-LEVEL errors
// (malformed body, missing path, missing asset, reserved root) map to
// `ok: false` / structured codes. This is the contract that lets an agent
// treat "compile failed" as data rather than as an opaque isError.
//
// This is the spine of the structure-edit loop: blueprint_add_variable /
// blueprint_add_function / blueprint_add_event all mark the Blueprint
// structurally modified, and the change only lands on the generated class once
// blueprint_compile runs. The expected loop is: structure edit -> compile ->
// (set_default / spawn / inspect).
//
// Mutating: compile rebuilds the generated class + bytecode and dirties the
// package, so it runs the full gate path (checkpoint -> compile -> validate ->
// delta); `paths_hint` MUST list the Blueprint package path (e.g.
// ['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set
// gate:"off" to bypass. The gate's post-mutation validate pass runs the
// standard package-scoped rules (e.g. missing_blueprint_parent /
// broken_soft_references) — Blueprint compile diagnostics are NOT a verify
// rule (they ride through as the messages[] array above). For a tight
// recompile loop that always returns the result, pass gate:"warn" or
// gate:"off".
//
// Fidelity: greenfield. No Unity Blueprint / Kismet twin (loose analogy only:
// Unity's compile_check / read_compile_errors WORKFLOW — different engine, no
// shared code). Behavior reference (read-only): Unreal-MCP's blueprint-compile
// for the FCompilerResultsLog silent compile + the FTokenizedMessage walk +
// the success:false-with-payload intent (mapped here to ok:true +
// succeeded:false so MCP isError stays false for ordinary diagnostics).
//
// Intentional deltas vs Unreal-MCP:
//   - Map Unreal-MCP's bSuccess:false + payload to ok:true + result.succeeded:
//     false so MCP isError stays false for ordinary compile diagnostics.
//   - Canonical MCP envelope + gate summary on compile.
//
// Route: live (POST /tools/unreal_open_mcp_blueprint_compile). Mutating.
export const blueprintCompile: Tool = {
  name: "unreal_open_mcp_blueprint_compile",
  description:
    "Compile a Blueprint and return a STRUCTURED error/warning list — the AI " +
    "feedback loop. `path` is the Blueprint asset object path (package-path " +
    "form also accepted — resolved in-memory first, then loaded). Compiles " +
    "via FKismetEditorUtilities::CompileBlueprint with a silent compiler log. " +
    "A FAILED compile is a NORMAL, expected result, NOT a transport failure: " +
    "the envelope stays ok:true and the result carries succeeded:false plus a " +
    "populated messages[] so you can read the diagnostics, fix the structure " +
    "via blueprint_add_variable / blueprint_modify_variable / " +
    "blueprint_add_function / blueprint_add_event, and recompile. Only " +
    "tool-level errors (missing path, missing asset, reserved root, malformed " +
    "body) map to ok:false. This is the spine of the structure-edit loop — a " +
    "member variable added via blueprint_add_variable, a function stub from " +
    "blueprint_add_function, or an event from blueprint_add_event only lands " +
    "on the generated class once blueprint_compile runs; the expected loop is " +
    "structure edit -> compile -> (set_default / spawn / inspect). Mutating: " +
    "compile rebuilds the generated class + bytecode and dirties the package, " +
    "so it runs the full gate path (checkpoint -> compile -> validate -> " +
    "delta); `paths_hint` MUST list the Blueprint package path (e.g. " +
    "['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set " +
    "gate:\"off\" to bypass. Result shape: { succeeded (bool), numErrors " +
    "(int), numWarnings (int), messages[] } where each message is " +
    "{ severity ('error'|'warning'|'info'), message, node (best-effort, may " +
    "be empty), graph (best-effort, may be empty) }. Error codes: " +
    "missing_parameter (path absent), blueprint_not_found (no Blueprint at " +
    "path), invalid_content_root (Blueprint under /Engine, /Script, /Temp), " +
    "invalid_parameter (malformed body). NOTE: a non-zero error count from " +
    "the compiler is NOT one of these codes — it rides through as " +
    "succeeded:false on an ok:true envelope.",
  inputSchema: {
    type: "object",
    required: ["path", "paths_hint"],
    properties: {
      path: {
        type: "string",
        description:
          "Blueprint asset object path — an object path " +
          "('/Game/Mcp/BP_Thing.BP_Thing') or a package path " +
          "('/Game/Mcp/BP_Thing'). Resolved in-memory first (a Blueprint " +
          "created this session but not yet saved), then loaded.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — the Blueprint package path(s) the mutation is " +
          "scoped to, fed to the gate as the checkpoint + validate hint. " +
          "REQUIRED for mutating tools (the gate refuses an empty hint with " +
          "paths_hint_required; there is no whole-project fallback). Set " +
          "gate:\"off\" to bypass the gate and skip the hint.",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint -> compile -> " +
          "validate -> delta and hard-fails on new Errors surfaced by the " +
          "package-scoped verify rules (Blueprint compile diagnostics are " +
          "NOT a verify rule — they ride through as the messages[] array on " +
          "an ok:true envelope); warn commits the compile but surfaces new " +
          "Errors as warnings; off skips the gate entirely (paths_hint " +
          "optional). For a tight recompile loop that always returns the " +
          "result, pass gate:\"warn\" or gate:\"off\".",
      },
    },
    additionalProperties: false,
  },
};
