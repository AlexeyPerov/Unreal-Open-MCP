// Source tool family for the bridge tool surface (P7.1).
//
// Two read-only tools that give an agent a first-class, JAILED way to inspect
// project C++ under <Project>/Source/:
//   - `unreal_open_mcp_source_read` — read a source file with optional 1-based
//     line slice and a soft max-lines cap. Returns numbered lines + truncation
//     metadata. Refuses directories and absurdly large files before reading.
//   - `unreal_open_mcp_source_list` — enumerate source files under Source/
//     (optionally scoped to a module folder), with a recursive flag and an
//     extension allow-list. Returns Source-relative paths + count + total bytes.
//
// Both tools are read-only (no gate path) and live-route only — there is no
// offline disk route for source inspection in P7 (that lands in P8).
//
// The jail helpers (GetProjectSourceRoot, ResolveJailedPath) are exported here
// (separate from the Register entry point) so the Automation specs in the
// sibling UnrealOpenMcpEditorTests module can exercise them directly, fast and
// deterministically, with an injectable temp JailRoot and no live editor.
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
 * The C++ source read/list tool family (P7.1).
 *
 * Read-only inspection + inventory of project source files under
 * <Project>/Source/. No writes, no compile — those land in later P7 sub-plans.
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
	 * Register the source family with @p Registry. Registers:
	 *   `unreal_open_mcp_source_read` (read-only),
	 *   `unreal_open_mcp_source_list` (read-only).
	 * First-registration-wins: a duplicate name is ignored by the registry.
	 */
	void Register(FUnrealOpenMcpToolRegistry& Registry);
}
