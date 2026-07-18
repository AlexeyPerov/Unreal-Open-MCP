// Gate meta-tool family — explicit checkpoint / validate / delta surfaces.
//
// Ported from Unity Open MCP packages/bridge/Editor/MetaTools/
// (ValidateEditTool.cs / CheckpointCreateTool.cs / DeltaTool.cs) at copy
// fidelity. Unity exposes these as standalone static classes; the Unreal port
// bundles them into one family file under MetaTools/ and registers all three
// via a single Register(Registry) entry point (mirrors the actor-family
// pattern used elsewhere in the bridge).
//
// P3.6 ships three read-only meta-tools (all registered with
// FUnrealOpenMcpToolMetadata::ReadOnly() so they bypass GatePolicy.Execute —
// the meta-tools PARTICIPATE in the gate workflow but must not recurse
// through it):
//
//   unreal_open_mcp_validate_edit
//     Scoped health check over `paths`. Auto-selects rules by extension; the
//     caller may narrow via `categories` / `include_rules` / `exclude_rules`.
//     Returns the issue list with stable code/severity fields + `passed`
//     (false when any Error fired). Mirrors Unity's validate_edit.
//
//   unreal_open_mcp_checkpoint_create
//     Capture a fingerprint over `paths` and store it in
//     FUnrealOpenMcpCheckpointStore. Returns the checkpoint id + a
//     per-rule {errors,warnings,issueKeys} summary. Empty `paths` snapshots
//     every registered rule over an empty scope (Unity parity: "whole project
//     summary" is expensive and intentionally not the default behavior of the
//     runner; the agent opts in by passing no paths).
//
//   unreal_open_mcp_delta
//     Compare the current validate scan against a stored checkpoint. Returns
//     `summary{newErrors,newWarnings,resolvedErrors,resolvedWarnings}` plus
//     the newIssueKeys / resolvedIssueKeys lists. A missing checkpoint is NOT
//     a tool failure — the store is session-scoped and a hot reload wipes it,
//     so the response carries `unavailable:true` + structured recovery
//     guidance (agentNextSteps) instead of `isError:true`.
//
// Storage contract: FUnrealOpenMcpCheckpointStore is process-lifetime and
// in-memory (see Gate/UnrealOpenMcpCheckpointStore.h). A hot reload / editor
// restart wipes every checkpoint — any checkpoint_id an agent holds is gone
// after a reload. Delta surfaces this honestly as `checkpointLostOnReload`
// when the store is empty AND a specific id was requested.
//
// Every handler registered here runs ON THE GAME THREAD (the HTTP server
// marshals dispatches through the GameThreadDispatcher).
#pragma once

#include "CoreMinimal.h"

class FUnrealOpenMcpToolRegistry;

/**
 * Register the gate meta-tool family with @p Registry. Each P3.6 meta-tool is
 * registered here so the module boot wires the whole family in one place.
 * First-registration-wins: a duplicate name is ignored by the registry.
 *
 * P3.6 registers:
 *   - `unreal_open_mcp_validate_edit`     (read-only; gate Off).
 *   - `unreal_open_mcp_checkpoint_create` (read-only; gate Off).
 *   - `unreal_open_mcp_delta`             (read-only; gate Off).
 *
 * All three are classified read-only so the dispatch policy runs them
 * directly (no GatePolicy.Execute recursion — pinned acceptance criterion).
 */
namespace FUnrealOpenMcpGateMetaTools
{
	void Register(FUnrealOpenMcpToolRegistry& Registry);
}
