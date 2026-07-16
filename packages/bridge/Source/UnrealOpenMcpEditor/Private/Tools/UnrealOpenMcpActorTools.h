// Actor-tool family registration for the bridge tool surface.
//
// P2.2 ships the first real read-only typed tool: `unreal_open_mcp_actor_find`.
// It locates actors in the editor world by ref (label → name → path) or lists
// them with filters, returning a structured `actors[]` array the agent can
// chain into later actor tools. Read-only — no level mutation, no gate path.
//
// P2.3 ships the first mutating typed tool: `unreal_open_mcp_actor_create`.
// It spawns an actor in the current editor level from a native class path or a
// Blueprint asset path, optionally setting label / location / rotation / parent
// attachment, and returns the new actor's `ActorData` for chaining. Wrapped in
// an `FScopedTransaction` for editor Undo support. The schema carries
// forward-compat `paths_hint` / `gate` fields but gate execution is a no-op
// until P3.5 (per the P2.3 plan — schema forward-compat only).
//
// Adapted from Unity Open MCP's GameObjectsTools
// (packages/bridge/Editor/TypedTools/GameObjectsTools.cs — the Find + Create
// handlers) at adapt fidelity:
//   - Addressing surface is string-only (Unreal has no global int instance id;
//     the `actor` ref string replaces Unity's instance_id/path/name trio).
//   - The targeted-miss contract is copied from Unity's gameobject-find: a
//     targeted lookup that resolves nothing returns {ok:true, notFound:true}
//     (it is NOT an error), so an agent can distinguish "found nothing" from
//     "the request was malformed".
//   - List filters narrow to `class` + `name_contains` (Unreal tags/layer are
//     deferred to a later phase per the P2.2 plan).
//   - Create: class path / Blueprint asset path replaces Unity's fixed
//     `primitive_type` enum; `FScopedTransaction` replaces Unity's
//     `Undo.RegisterCreatedObjectUndo`; attachment targets the parent actor's
//     root via `AttachToActor` instead of Unity's transform hierarchy.
//
// Behavior reference (read-only): Unreal-MCP's actor handlers
// (UnrealMcpActorTools.cpp) — TActorIterator sweep, class/name filtering, the
// label-vs-name reader, SpawnActor + FScopedTransaction + parent attachment,
// and the resolve-parent-before-spawn ordering were studied for correct Unreal
// editor API usage.
//
// Every handler registered here runs ON THE GAME THREAD (the HTTP server
// marshals dispatchs through the GameThreadDispatcher).
#pragma once

#include "CoreMinimal.h"

class FUnrealOpenMcpToolRegistry;

/**
 * Register the actor-tool family with @p Registry. Each P2.2+ actor tool is
 * registered here so the module boot wires the whole family in one place.
 * First-registration-wins: a duplicate name is ignored by the registry.
 *
 * P2.2 registers: `unreal_open_mcp_actor_find` (read-only).
 * P2.3 registers: `unreal_open_mcp_actor_create` (mutating; gate deferred).
 */
namespace FUnrealOpenMcpActorTools
{
	void Register(FUnrealOpenMcpToolRegistry& Registry);
}
