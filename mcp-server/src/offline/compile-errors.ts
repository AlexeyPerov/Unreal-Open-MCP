// Unreal compile-diagnostic extraction for the offline
// `unreal_open_mcp_read_compile_errors` tool.
//
// Mirrors the bridge's `ParseDiagnostics`
// (`packages/bridge/.../UnrealOpenMcpSourceTools.cpp`) so an offline log read
// produces the SAME `{ file, line, severity, message }` shape the live
// `source_compile` tool returns — an agent fixes a diagnostic the same way
// regardless of whether it came from a live compile or a dead-bridge log tail.
//
// Recognized formats (byte-for-byte the regexes the bridge uses):
//   - MSVC  `file(line): error Cxxxx: msg`
//          `file(line,col): warning Cxxxx: msg`
//          `file(line): fatal error Cxxxx: msg` (severity normalized to "error")
//   - clang `file:line:col: error: msg`
//          `file:line:col: fatal error: msg` (severity normalized to "error")
//
// Adapted from Unity Open MCP's mcp-server/src/compiler-errors.ts (copy
// fidelity for the extractor structure + dedupe; intentional delta in the
// regexes — Unity parses CSxxxx, Unreal parses MSVC Cxxxx + clang). No new deps.

/** Maximum number of distinct errors surfaced. Bounded so a giant wall of
 *  errors can't blow up the tool response; the agent can fix-and-recheck.
 *  Matches Unity's MAX_COMPILER_ERRORS. */
export const MAX_COMPILE_ERRORS = 50;

/**
 * A single compiler diagnostic parsed from an Unreal log tail. Shape mirrors
 * the bridge's FSourceDiagnostic (and the `source_compile` diagnostics[] entry)
 * so an agent treats an offline and a live diagnostic identically.
 */
export interface CompileError {
  /** Source file path as emitted by the compiler (verbatim — backslashes on
   *  MSVC, forward slashes on clang). Not jail-normalized. */
  file: string;
  /** 1-based line number, or 0 when unparsed. */
  line: number;
  /** Lower-case "error" or "warning" (a `fatal error` is normalized to "error"
   *  so the AI loop keys off severity without a third bucket). */
  severity: "error" | "warning";
  /** Compiler message text (trimmed). Carries the diagnostic code, e.g.
   *  "C2065: 'Foo': undeclared identifier". */
  message: string;
}

/**
 * MSVC diagnostic line. Byte-for-byte the bridge's MSVC regex (anchored ^…$
 * per line, optional column, optional `fatal ` prefix, error|warning).
 *
 *   group 1: file path (lazy — up to the last `(` before `):`)
 *   group 2: line number
 *   group 3: severity (error | warning) — the optional `fatal ` prefix is
 *            consumed and dropped
 *   group 4: message (the Cxxxx code + text)
 */
const MSVC_RE =
  /^\s*(.+?)\((\d+)(?:,\d+)?\)\s*:\s*(?:fatal\s+)?(error|warning)\s+(.+?)\s*$/;

/**
 * Clang diagnostic line. Byte-for-byte the bridge's clang regex.
 *
 *   group 1: file path
 *   group 2: line number
 *   group 3: severity (error | warning)
 *   group 4: message
 */
const CLANG_RE =
  /^\s*(.+?):(\d+):(?:\d+:)?\s*(?:fatal\s+)?(error|warning):\s*(.+?)\s*$/;

/**
 * Extract MSVC + clang diagnostics from an Unreal log tail (or a UBT build
 * output blob) into structured {@link CompileError} records, deduped by
 * (file|line|severity|message), in first-seen order, capped at
 * {@link MAX_COMPILE_ERRORS}.
 *
 * This is the offline twin of the bridge's `ParseDiagnostics`. Bare
 * `LINK : fatal` rows are NOT emitted (they are link-stage, not compiler-stage
 * — the same policy the bridge applies; a link failure is modeled separately
 * by the compile tool's `success:false` + `compile_clean:true` split, which
 * does not apply to the offline log reader).
 */
export function extractCompileErrors(output: string): CompileError[] {
  if (!output) return [];
  const seen = new Set<string>();
  const errors: CompileError[] = [];
  const lines = output.split(/\r?\n/);
  for (const rawLine of lines) {
    if (errors.length >= MAX_COMPILE_ERRORS) break;
    if (!rawLine) continue;
    // MSVC first (more specific `file(line):` shape); clang as the fallback.
    let m = MSVC_RE.exec(rawLine);
    if (!m) m = CLANG_RE.exec(rawLine);
    if (!m) continue;
    const file = (m[1] ?? "").trim();
    const line = m[2] ? parseInt(m[2], 10) : 0;
    const severity = (m[3] ?? "error").toLowerCase() as "error" | "warning";
    const message = (m[4] ?? "").trim();
    // Dedupe key — a UBT run often emits the same diagnostic on stdout AND
    // stderr, and the same error from a header surfaces once per including cpp.
    const key = `${file}|${line}|${severity}|${message}`;
    if (seen.has(key)) continue;
    seen.add(key);
    errors.push({ file, line, severity, message });
  }
  return errors;
}

/**
 * Project-health summary from an Unreal log tail. Adapted from Unity's
 * `summarizeProjectHealth` shape (project_unhealthy / compile_failed /
 * no_errors_found status tokens), narrowed to the compiler-error surface —
 * Unreal does not have Unity's package-manager / assembly-resolution red flags
 * in the same log channel (those surface as UBT errors, which the compiler
 * parser already captures).
 */
export interface ProjectHealth {
  /** `true` when at least one error-severity diagnostic was found. */
  unhealthy: boolean;
  /** One-line triage summary (empty when healthy). */
  headline: string;
  /** Structured compiler diagnostics (errors + warnings). */
  errors: CompileError[];
}

/**
 * Build a health summary from an Unreal log tail. `unhealthy` is true iff at
 * least one error-severity diagnostic was parsed; warnings alone are not
 * unhealthy (they surface in `errors` for visibility but do not flip the flag).
 *
 * Status mapping (mirrors Unity's compile_failed / project_unhealthy /
 * no_errors_found tokens):
 *   - compile_failed    — at least one error-severity diagnostic
 *   - no_errors_found   — no error-severity diagnostics (warnings may be present)
 *
 * (Unreal does not use Unity's `project_unhealthy` token — Unity reserves it
 * for package/assembly red flags the compiler parser does not catch; Unreal's
 * equivalent red flags ARE compiler errors and surface as compile_failed.)
 */
export function summarizeProjectHealth(logTail: string): ProjectHealth {
  const errors = extractCompileErrors(logTail);
  const errorCount = errors.filter((e) => e.severity === "error").length;
  const unhealthy = errorCount > 0;
  const headline = unhealthy
    ? `${errorCount} compile error${errorCount === 1 ? "" : "s"} found in the editor log tail.`
    : "";
  return { unhealthy, headline, errors };
}

/** The coarse status token the read_compile_errors tool reports. */
export type CompileErrorStatus =
  | "compile_failed"
  | "no_errors_found"
  | "log_not_found";

/** Derive the status token from a health summary + whether a log was found. */
export function compileErrorStatus(
  health: ProjectHealth | null,
  logFound: boolean,
): CompileErrorStatus {
  if (!logFound) return "log_not_found";
  return health && health.unhealthy ? "compile_failed" : "no_errors_found";
}
