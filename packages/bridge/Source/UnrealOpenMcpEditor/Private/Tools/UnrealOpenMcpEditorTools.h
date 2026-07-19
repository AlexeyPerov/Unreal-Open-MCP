// Editor-application + selection tool family for the bridge tool surface.
//
// P5.1 ships the editor application state pair — the truthful PIE / editor
// play-state model + bounded start/stop/pause/resume commands:
//   - `unreal_open_mcp_editor_application_get_state` — read-only snapshot of the
//     editor play state (isPlaying / isPaused / isSimulating) + the current
//     editor map (persistent-level package path + short name). Read from
//     GEditor->PlayWorld / bIsSimulatingInEditor / the editor world context.
//   - `unreal_open_mcp_editor_application_set_state` — drive PIE transitions by
//     an `action` enum (start / stop / pause / resume). start/stop go through
//     GEditor->RequestPlaySession / RequestEndPlayMap, which are LATENT (the
//     transition happens on a later editor tick), so the tool returns a
//     structured `{ action, pending:true }` and the agent polls get-state to
//     observe the settled transition (the Unreal-MCP honesty pattern — never
//     claim an unobserved transition). pause/resume drive the PIE player
//     controller's pause immediately (pending:false). Invalid transitions
//     (start while already playing, stop/pause/resume while not playing, pause
//     while already paused, resume while not paused) return a structured
//     `invalid_transition` — no silent restart, selection/state unchanged.
//
// P5.2 ships the editor selection pair — read/drive the editor actor selection:
//   - `unreal_open_mcp_editor_selection_get` — read-only list of the currently
//     selected actors as P2 identity refs ({ name, label, class, path }),
//     matching the actor-family identity shape so an agent can chain a selection
//     read straight into actor_modify / screenshot / inspect.
//   - `unreal_open_mcp_editor_selection_set` — replace the selection by refs, or
//     clear it with an explicit `clear:true`. Resolve-all-before-mutate: every
//     ref is resolved up front and a single bad ref aborts the whole call with
//     `actor_not_found` BEFORE any selection change, so a partial/half-applied
//     selection never lands. An empty call WITHOUT `clear:true` is refused with
//     `missing_parameter` (never a silent deselect). The mutation is wrapped in
//     an FScopedTransaction and commits with a single NoteSelectionChange.
//
// Gate: the two read tools carry no gate; the two mutators
// (editor_application_set_state, editor_selection_set) register with
// `FUnrealOpenMcpToolMetadata::Mutating()` so the dispatch path routes every
// call through `FUnrealOpenMcpGatePolicy::Execute` and `paths_hint` is
// mandatory (the current map package and/or the selected actor paths / project
// scope). There is no whole-project silent fallback — an empty hint fails with
// paths_hint_required unless the caller sets gate:"off".
//
// Adapted from Unity Open MCP's editor-state + selection tools
// (mcp-server/src/tools/editor-set-state.ts, editor-status.ts, selection-get.ts,
// selection-set.ts; packages/bridge/Editor/TypedTools/
// EditorConsoleSelectionTools.cs) at adapt fidelity:
//   - Verb shape is an `action` enum (start/stop/pause/resume) + a latent
//     `pending` result, NOT Unity's `is_playing` boolean — the Unreal PIE
//     lifecycle is request-then-tick, so an honest pending/poll contract
//     replaces a synchronous is_playing set.
//   - `isSimulating` is an Unreal-specific Simulate-In-Editor flag with no
//     direct Unity twin.
//   - Selection is actor-only (Unreal editor actor selection via USelection),
//     addressed with the same string refs as the actor family (label → name →
//     path via FUnrealOpenMcpObjectRef::ResolveActor); Unity's GameObject /
//     component multi-select has no direct parity here.
//   - Explicit-clear-required copies Unreal-MCP's empty-arg refuse so Unity's
//     empty-array "wipe" semantics never silently deselect.
//
// Behavior reference (read-only): Unreal-MCP's editor handlers
// (UnrealMcpEditorTools.cpp — editor-application-get-state / -set-state,
// editor-selection-get / -set) for the correct PIE request APIs
// (RequestPlaySession / RequestEndPlayMap), the pending/poll contract, and the
// USelection resolve/clear rules.
//
// Every handler registered here runs ON THE GAME THREAD (the HTTP server
// marshals dispatch through the GameThreadDispatcher), so the handlers touch
// GEditor / UWorld / USelection freely.
#pragma once

#include "CoreMinimal.h"

class FUnrealOpenMcpToolRegistry;

/**
 * Register the editor-application + selection tool family with @p Registry.
 * The whole family is wired in one place so the module boot registers it with a
 * single call. First-registration-wins: a duplicate name is ignored by the
 * registry.
 *
 * Registers: `unreal_open_mcp_editor_application_get_state` (read-only),
 *             `unreal_open_mcp_editor_application_set_state` (mutating; gate
 *             Enforce; paths_hint required),
 *             `unreal_open_mcp_editor_selection_get` (read-only),
 *             `unreal_open_mcp_editor_selection_set` (mutating; gate Enforce;
 *             paths_hint required).
 */
namespace FUnrealOpenMcpEditorTools
{
	void Register(FUnrealOpenMcpToolRegistry& Registry);
}
