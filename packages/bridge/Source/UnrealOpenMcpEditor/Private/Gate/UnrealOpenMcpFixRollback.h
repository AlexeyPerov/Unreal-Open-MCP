// FFixRollback — file-level snapshot helper for the apply_fix gate runner.
//
// Ported from Unity Open MCP packages/verify/Editor/Fixes/FixRollback.cs at
// copy fidelity, adapted to Unreal's content-package layout:
//   - Unity snapshots Assets/<path> + its companion .meta (every asset has
//     one, and several fixes rewrite/delete the .meta).
//   - Unreal snapshots the on-disk .uasset / .umap for a content package,
//     plus the companion .uexp / .ubulk blobs UE writes alongside (a fix
//     that mutates a soft pointer can grow the bulk payload). Source-file
//     fixes (compile_errors) are not Safe in v1, so the rollback surface is
//     content-only.
//
// The snapshot is best-effort: a path that does not exist on disk when
// Snapshot() runs is recorded as "did-not-exist" so a later Restore() knows
// to delete a file the fix may have created. A path that did exist is
// captured as raw bytes so Restore() can write them back verbatim.
//
// Load-bearing invariant: Restore() MUST return the project to its pre-fix
// state for every snapshotted path, INCLUDING a fix that created a file the
// snapshot recorded as did-not-exist. The ApplyFixGateRunner relies on this
// to honor the gate's trust contract (a fix that introduces new errors under
// Enforce is rolled back; no project change remains).
//
// Scope discipline: the helper snapshots ONLY the paths the runner predicts
// the fix may touch (the issue's asset path + its on-disk siblings). The
// runner never calls Snapshot() with paths outside the project's Content
// tree, and Restore() only touches snapshotted paths.
#pragma once

#include "CoreMinimal.h"

/**
 * Result of a Restore() call — surfaced so the apply_fix envelope can report
 * the restored paths to the agent.
 */
struct UNREALOPENMCPEDITOR_API FUnrealOpenMcpFixRollbackRestore
{
	/** Paths whose pre-fix bytes were written back (existed before, rewritten). */
	TArray<FString> RestoredPaths;
	/** Paths that did not exist pre-fix and were deleted by Restore(). */
	TArray<FString> DeletedPaths;
};

/**
 * File-level snapshot + restore helper. See header rationale.
 *
// Lifetime: stack-scoped by FUnrealOpenMcpApplyFixGateRunner around one apply
// attempt. Snapshot() captures; Restore() reverts; Discard() releases the
// captured bytes without writing anything back (the apply committed cleanly).
 */
class UNREALOPENMCPEDITOR_API FUnrealOpenMcpFixRollback
{
public:
	FUnrealOpenMcpFixRollback() = default;
	~FUnrealOpenMcpFixRollback() = default;

	/** Non-copyable (owns a byte buffer) / non-movable (referenced by the runner
	 *  via the address-of capture in the runner's try/catch). */
	FUnrealOpenMcpFixRollback(const FUnrealOpenMcpFixRollback&) = delete;
	FUnrealOpenMcpFixRollback& operator=(const FUnrealOpenMcpFixRollback&) = delete;

	/**
	 * Capture the on-disk bytes of every path in Paths. A path that does not
	 * exist is recorded as did-not-exist (Restore() will delete it if the fix
	 * created it). Idempotent — re-calling with overlapping paths is safe.
	 *
	 * @param Paths  absolute filesystem paths to snapshot. The runner resolves
	 *               content package names to absolute paths before calling.
	 */
	void Snapshot(const TArray<FString>& Paths);

	/** True when Snapshot() captured at least one path. */
	bool HasSnapshot() const { return bHasSnapshot; }

	/**
	 * Restore every snapshotted path to its pre-fix state. Returns the
	 * restored/deleted path lists so the envelope can surface them.
	 *
	 * Safe to call when !HasSnapshot() — returns an empty result.
	 */
	FUnrealOpenMcpFixRollbackRestore Restore();

	/** Release captured bytes without restoring. Safe to call when !HasSnapshot(). */
	void Discard();

private:
	/** One captured path. */
	struct FEntry
	{
		/** Absolute filesystem path. */
		FString Path;
		/** Raw bytes when the file existed pre-fix; empty when bExisted is false. */
		TArray<uint8> Bytes;
		/** False when the file did not exist pre-fix (Restore deletes it). */
		bool bExisted = false;
	};

	TArray<FEntry> Entries;
	bool bHasSnapshot = false;
};
