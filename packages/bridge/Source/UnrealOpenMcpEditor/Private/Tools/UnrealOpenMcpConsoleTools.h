// Console tool family for the bridge tool surface (P5.3).
//
// Three tools over the bounded GLog ring buffer (FUnrealOpenMcpLogCollector)
// and the engine console exec:
//   - `unreal_open_mcp_console_get_logs` — read-only filtered slice of the ring
//     (verbosity min-severity / category / substring + a bounded limit). Each
//     entry carries { sequence, verbosity, category, message, timestamp }.
//   - `unreal_open_mcp_console_clear_logs` — empty the ring, returning the
//     removed count. Classified read-only (buffer-local; it mutates no project
//     / editor state) so it dispatches directly without the gate.
//   - `unreal_open_mcp_console_run_command` — run a console command via
//     GEngine->Exec against the editor world, returning { command, output,
//     handled }. Mutating (a console command can change project/editor state) —
//     registered with `FUnrealOpenMcpToolMetadata::Mutating()`; `paths_hint` is
//     mandatory. This is a destructive surface (same accepted-risk class as
//     reflection_method_call): the tool description says so and the gate is the
//     safeguard, not a per-command allow-list.
//
// Adapted from Unity Open MCP's console-log / console-clear / read-console
// tools at adapt fidelity: the capture surface is the Unreal GLog sink (a
// FOutputDevice ring) rather than Unity's Console window API, but the agent
// filter workflow (min severity + category + substring + limit) is the same.
// The run-command tool has no direct Unity twin; it follows the Unreal-MCP
// console-run-command behavior (GEngine->Exec with captured output).
//
// Behavior reference (read-only): Unreal-MCP's console handlers
// (UnrealMcpEditorTools.cpp — console-get-logs / -clear-logs / -run-command)
// and its UnrealMcpLogCollector for the GLog sink + exec-capture behavior.
//
// Every handler runs ON THE GAME THREAD (the HTTP server marshals dispatch
// through the GameThreadDispatcher).
#pragma once

#include "CoreMinimal.h"

class FUnrealOpenMcpToolRegistry;

/**
 * Register the console tool family with @p Registry. Registers:
 *   `unreal_open_mcp_console_get_logs`   (read-only),
 *   `unreal_open_mcp_console_clear_logs` (read-only; buffer-local),
 *   `unreal_open_mcp_console_run_command`(mutating; gate Enforce; paths_hint
 *   required).
 * First-registration-wins: a duplicate name is ignored by the registry.
 */
namespace FUnrealOpenMcpConsoleTools
{
	void Register(FUnrealOpenMcpToolRegistry& Registry);
}
