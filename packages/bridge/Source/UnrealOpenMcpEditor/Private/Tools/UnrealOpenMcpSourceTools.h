// Source tool family for the bridge tool surface (P7.1 + P7.2 + P7.3).
//
// P7.1 — two read-only tools that give an agent a first-class, JAILED way to
// inspect project C++ under <Project>/Source/:
//   - `unreal_open_mcp_source_read` — read a source file with optional 1-based
//     line slice and a soft max-lines cap. Returns numbered lines + truncation
//     metadata. Refuses directories and absurdly large files before reading.
//   - `unreal_open_mcp_source_list` — enumerate source files under Source/
//     (optionally scoped to a module folder), with a recursive flag and an
//     extension allow-list. Returns Source-relative paths + count + total bytes.
// Both read-only tools are gate-free (no checkpoint / validate / delta) and
// live-route only — there is no offline disk route for source inspection in P7
// (that lands in P8).
//
// P7.2 — three JAILED mutating tools that scaffold / edit / remove source files
// so the compile loop has something to mutate:
//   - `unreal_open_mcp_source_create_class` — scaffold a header + cpp from
//     parent-kind templates (UObject / Actor / ActorComponent / None) into an
//     existing module folder. Derives the U/A/F prefix, emits the MODULE_API
//     macro, refuses overwrite unless `force`, rolls back the header if the cpp
//     write fails.
//   - `unreal_open_mcp_source_update` — full-file replace or 1-based inclusive
//     line-range splice on an existing file; preserves the detected EOL when
//     splicing.
//   - `unreal_open_mcp_source_delete` — delete a single file; refuses
//     directories.
// All three are mutating (gate Enforce, `paths_hint` required — the dispatcher
// enforces `paths_hint` BEFORE the handler runs, so the handlers themselves do
// not read it). Writes only touch the project `Source/` jail.
//
// P7.3 — `unreal_open_mcp_source_compile`, the AI feedback loop for C++. It
// prefers Live Coding when the editor is interactive + Live Coding is live,
// otherwise invokes UnrealBuildTool on the project's Editor target and parses
// MSVC + clang stdout/stderr into a STRUCTURED `{file,line,severity,message}`
// report. `success` (process return 0) is reported SEPARATELY from
// `compile_clean` (zero compiler errors): a loaded editor holds its module DLL,
// so a UBT relink fails to write it — but compiler errors are emitted BEFORE
// the link stage, so they are unaffected by the lock and the AI loop keys off
// them. A failed compile is a NORMAL result (`ok:true` + result fields), NOT a
// transport failure. Mutating (gate Enforce, `paths_hint` required).
//
// The jail helpers (GetProjectSourceRoot, ResolveJailedPath) and the diagnostic
// parser (FSourceDiagnostic, ParseDiagnostics) are exported here (separate from
// the Register entry point) so the Automation specs in the sibling
// UnrealOpenMcpEditorTests module can exercise them directly, fast and
// deterministically, with an injectable temp JailRoot / canned build output and
// no live editor / no UBT invocation.
//
// Adapted from Unity Open MCP's script-read (line slice + max_lines + project-
// root refusal) at adapt fidelity: the file read shape is Unity's, the jail is
// Unreal-specific (Source/ only, not Assets/Packages). The directory listing
// adapts the Unreal-MCP source-list behavior (scoped inventory + extension
// allow-list). Unity Open MCP is the canonical reference; Unreal-MCP is a
// specs-only behavior reference, never named in tracked docs.
//
// Every handler runs ON THE GAME THREAD (the HTTP server marshals dispatch
// through the GameThreadDispatcher).
#pragma once

#include "CoreMinimal.h"

class FUnrealOpenMcpToolRegistry;

/**
 * The C++ source tool family (P7.1 read/list + P7.2 create/update/delete +
 * P7.3 compile).
 *
 * Read-only inspection + inventory of project source files under
 * <Project>/Source/, plus JAILED mutating CRUD so an agent can scaffold and
 * edit C++ for the compile loop, plus the compile tool itself that closes the
 * AI feedback loop with a structured diagnostic report.
 */
namespace FUnrealOpenMcpSourceTools
{
	/** Outcome of resolving a caller-supplied path against the project Source/
	 *  jail. Exported so the Automation spec can drive the jail directly with an
	 *  injectable temp root. */
	struct UNREALOPENMCPEDITOR_API FJailedPath
	{
		/** True when the path resolved to a canonical absolute path inside the
		 *  jail root. */
		bool bOk = false;
		/** Canonicalized absolute path (valid only when bOk). */
		FString FullPath;
		/** Path relative to the jail root, '/'-separated (valid only when bOk). */
		FString RelPath;
		/** Human-readable reason (valid only when !bOk). */
		FString Error;
	};

	/** Absolute, normalized jail root = <Project>/Source for the currently
	 *  loaded project. */
	UNREALOPENMCPEDITOR_API FString GetProjectSourceRoot();

	/**
	 * Canonicalize @p InPath (relative to the jail root, or absolute) and confirm
	 * it stays inside the jail. Rejects `..` traversal and absolute paths that
	 * escape, and — best effort — a junction / symlink whose on-disk target
	 * escapes the jail. @p JailRoot lets specs supply a temp root.
	 */
	UNREALOPENMCPEDITOR_API FJailedPath ResolveJailedPath(const FString& JailRoot, const FString& InPath);

	/**
	 * A single parsed compiler diagnostic row from UBT stdout/stderr. The fields
	 * map 1:1 to the JSON `diagnostics[]` entries `source_compile` returns.
	 * Exported so the Automation spec can drive the parser directly with canned
	 * MSVC / clang fixtures — no UBT invocation, no live editor.
	 */
	struct UNREALOPENMCPEDITOR_API FSourceDiagnostic
	{
		/** Source file path as emitted by the compiler (verbatim — backslashes
		 *  on MSVC, forward slashes on clang). Not jail-normalized. */
		FString File;
		/** 1-based line number (0 if unparsed). */
		int32 Line = 0;
		/** Lower-case "error" or "warning" (a `fatal error` is normalized to
		 *  "error" so the AI loop keys off severity without a third bucket). */
		FString Severity;
		/** Compiler message text (trimmed). Carries the diagnostic code, e.g.
		 *  "C2065: 'Foo': undeclared identifier". */
		FString Message;
	};

	/**
	 * Parse MSVC + clang diagnostic lines out of a UBT build output blob into
	 * @p OutDiagnostics. Recognizes:
	 *   - MSVC  `file(line): error Cxxxx: msg`
	 *          `file(line,col): warning Cxxxx: msg`
	 *          `file(line): fatal error Cxxxx: msg` (severity normalized to
	 *           "error")
	 *   - clang `file:line:col: error: msg`
	 *          `file:line:col: fatal error: msg` (severity normalized to
	 *           "error")
	 * Deduplicates by (file|line|severity|message). Bare `LINK : fatal` rows
	 * are NOT emitted (they are link-stage, not compiler-stage, and the locked-
	 * DLL relink failure is already modeled by `success:false` + `compile_clean:
	 * true`). Exported for deterministic Automation fixtures.
	 */
	UNREALOPENMCPEDITOR_API void ParseDiagnostics(const FString& BuildOutput, TArray<FSourceDiagnostic>& OutDiagnostics);

	/**
	 * Register the source family with @p Registry. Registers:
	 *   `unreal_open_mcp_source_read` (read-only),
	 *   `unreal_open_mcp_source_list` (read-only),
	 *   `unreal_open_mcp_source_create_class` (mutating; gate Enforce),
	 *   `unreal_open_mcp_source_update` (mutating; gate Enforce),
	 *   `unreal_open_mcp_source_delete` (mutating; gate Enforce),
	 *   `unreal_open_mcp_source_compile` (mutating; gate Enforce).
	 * First-registration-wins: a duplicate name is ignored by the registry.
	 */
	void Register(FUnrealOpenMcpToolRegistry& Registry);
}
