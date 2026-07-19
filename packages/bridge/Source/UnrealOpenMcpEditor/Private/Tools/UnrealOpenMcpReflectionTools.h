// Reflection tool family for the bridge tool surface (P5.4).
//
// First-party UFunction discovery + safety-gated ProcessEvent invoke. No
// ReflectorNet, no McpPlugin — just the engine's own reflection
// (TFieldIterator<UFunction> / UObject::FindFunction / UObject::ProcessEvent).
//
//   - `unreal_open_mcp_reflection_method_find` — list the UFunctions on a class
//     (native class path, Blueprint asset/generated-class path, or short type
//     name) with a signature + flag descriptor per method. Overrides are
//     de-duped most-derived-wins. A `name` substring filter + a `limit` bound
//     the result; `matched` vs `returned` report the honest counts.
//   - `unreal_open_mcp_reflection_method_call` — invoke a method by `method`
//     name on either a live `target` (an actor/object ref) XOR a `class` (the
//     class default object). The param frame is built from the `args` JSON map
//     via the same FProperty ⇄ JSON codec the property tools use; the return
//     value + any out-params are read back into the result. SAFETY GATE: only
//     BlueprintCallable or CallInEditor functions may be invoked; the CDO path
//     additionally requires the function be static or CallInEditor (calling an
//     instance method on a shared CDO is unsafe). A non-callable function is
//     rejected with method_not_callable BEFORE any invoke.
//
// Accepted risk: a BlueprintCallable / CallInEditor function can still be
// destructive — this is the same accepted-risk class as console_run_command.
// v1 does NOT ship a per-function denylist; the flag allow-list + the mandatory
// gate (paths_hint) are the safeguards, and the tool description says so.
//
// Adapted from Unity Open MCP's find-members / invoke-method tools (schemas +
// agent workflow) at adapt fidelity: the discovery unit is a UFunction (not a
// C# member), the safety model is Unreal UFunction flags (not C# visibility),
// and the target XOR class (instance vs CDO) split is the Unreal pattern.
// Behavior reference (read-only): Unreal-MCP's reflection handlers
// (UnrealMcpEditorTools.cpp — reflection-method-find / -call + safety helpers).
//
// Every handler runs ON THE GAME THREAD (the HTTP server marshals dispatch
// through the GameThreadDispatcher), so ProcessEvent + the UObject graph are
// touched safely.
#pragma once

#include "CoreMinimal.h"

class FUnrealOpenMcpToolRegistry;

/**
 * Register the reflection tool family with @p Registry. Registers:
 *   `unreal_open_mcp_reflection_method_find` (read-only),
 *   `unreal_open_mcp_reflection_method_call` (mutating; gate Enforce; paths_hint
 *   required).
 * First-registration-wins: a duplicate name is ignored by the registry.
 */
namespace FUnrealOpenMcpReflectionTools
{
	void Register(FUnrealOpenMcpToolRegistry& Registry);
}
