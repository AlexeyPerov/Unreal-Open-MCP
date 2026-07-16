// Level dirty guard — the Unreal analog of Unity Open MCP's SceneDirtyGuard
// (packages/bridge/Editor/Bridge/SceneDirtyGuard.cs).
//
// A destructive level operation (open / create / unload) discards unsaved edits
// to the current editor world. Under the interactive editor the engine surfaces
// a save-prompt modal dialog; under -unattended / -nullrhi automation that
// dialog is suppressed, so an agent-driven flow would silently lose work. The
// guard closes that gap: before a destructive op runs, the caller asks
// `AreDirtyMapsPresent()` and can refuse with a structured `level_dirty` error
// (or bypass via an `ignore_dirty` flag) instead of relying on UI.
//
// Detection uses UEditorLoadingAndSavingUtils::GetDirtyMapPackages (UnrealEd)
// — the same surface the editor's own save-prompt keys off — so the guard and
// the editor agree on what "dirty" means. This is bridge-tracked + package
// dirty check (the P2.6 plan's "Dirty tracking" row): no persistent bridge-side
// dirty map is maintained, the engine's package dirty state is the source of
// truth and is probed per call.
//
// Every method here runs ON THE GAME THREAD (it touches GEditor / the package
// graph). The HTTP server marshals every tool dispatch through the
// GameThreadDispatcher before a handler is invoked.
#pragma once

#include "CoreMinimal.h"

/**
 * Level dirty-state guard shared by the level-tool family. Read-only probes —
 * none of these mutate the world or any package. Lives in a self-contained
 * header so every destructive level tool (open, future create) reuses the same
 * dirty check and keeps the guard's semantics identical across the family.
 */
namespace FUnrealOpenMcpLevelDirtyGuard
{
	/**
	 * True when one or more map packages in the current editor world are dirty
	 * (have unsaved edits). Returns false outside the editor / when no map is
	 * dirty. Mirrors the editor's own save-prompt trigger so an agent-driven
	 * destructive op and an interactive op agree on what would be lost.
	 */
	UNREALOPENMCPEDITOR_API bool AreDirtyMapsPresent();

	/**
	 * The long package names of every dirty map package in the current editor
	 * world (empty when none). Surfaced as a structured `discardedDirtyLevels`
	 * note so a caller that bypasses the guard (ignore_dirty) still learns which
	 * unsaved edits a destructive op dropped — under -unattended that note is
	 * the only signal, the engine's confirm dialog is suppressed.
	 */
	UNREALOPENMCPEDITOR_API TArray<FString> DirtyMapPackageNames();
}
