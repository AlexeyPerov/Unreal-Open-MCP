// FUnrealOpenMcpCheckpointStore — session-lifetime checkpoint history.
//
// Ported from Unity Open MCP packages/bridge/Editor/Gate/CheckpointStore.cs at
// copy fidelity. Stores gate-run checkpoints in-memory so the meta-tools that
// land in P3.6 (delta / checkpoint_create / validate_edit) can delta-compare
// against a recent checkpoint without re-running the pre-mutation snapshot.
//
// STORAGE CONTRACT (v1): checkpoints are process-lifetime and in-memory. A
// hot reload / editor restart wipes every checkpoint — any checkpoint_id an
// agent holds is gone after a reload. Disk persistence across reloads is
// explicitly backlog. v1 contract: honest error + doc, not persistence.
//
// LRU eviction at capacity 20 is a SEPARATE concern: it drops the least-
// recently-accessed entry when the store is full, NOT on reload. Reload
// empties the store entirely; LRU only trims under pressure.
#pragma once

#include "CoreMinimal.h"

// Forward declaration of the verify type so this header does not pull verify
// into every include path — the bridge hard-depends on verify (Build.cs), but
// keeping the include surface narrow means a verify rename touches only the
// adapter TU.
struct FCheckpointFingerprint;

/**
 * One checkpoint entry in the store. The fingerprint is the verify-side
 * snapshot (per-rule issue counts + key set); the Id/Label/Paths/TimestampUtc
 * fields are the bookkeeping the meta-tools surface.
 */
struct FUnrealOpenMcpCheckpointStoreEntry
{
	/** cp_xxxxxx token — the canonical id the gate and meta-tools key on. */
	FString CheckpointId;
	/** ISO-8601 UTC timestamp of the Store call. */
	FString TimestampUtc;
	/** LRU access clock. Seeded from TimestampUtc on Store; refreshed on every
	 *  Get so an actively-delta-compared checkpoint is not evicted purely by
	 *  insert count. ISO-8601 UTC so string compare is a valid ordering. */
	FString LastAccessedUtc;
	/** Optional agent-supplied label (P3.6 checkpoint_create). Empty for
	 *  gate-run mirrors. */
	FString Label;
	/** paths_hint the checkpoint was captured over. */
	TArray<FString> Paths;
	/** Rule ids that ran at checkpoint time. Empty for gate-run mirrors that
	 *  ran every registered rule. */
	TArray<FString> Categories;
	/** The verify fingerprint. */
	FCheckpointFingerprint Fingerprint;
};

/**
 * Session checkpoint store. Static-only; no instance state. Process-lifetime.
 *
 * The store is consulted by the P3.6 meta-tools (delta / validate_edit) and
 * populated by the gate flow (every gate run mirrors its checkpoint here) and
 * by the P3.6 checkpoint_create meta-tool. P3.5 wires only the gate-mirror
 * Store call + the static API; the Get / Recent surfaces light up when the
 * meta-tools that need them land.
 */
struct UNREALOPENMCPEDITOR_API FUnrealOpenMcpCheckpointStore
{
private:
	FUnrealOpenMcpCheckpointStore() = delete;

public:
	/** Max entries before LRU eviction kicks in. Mirrors Unity's
	 *  DefaultCapacity. */
	static constexpr int32 DefaultCapacity = 20;

	/**
	 * Store an entry. Overwrites a previous entry with the same CheckpointId
	 * (latest-data-wins; recency refreshed so the overwritten entry is moved
	 * to the tail). LRU-evicts the least-recently-accessed entry when the store
	 * is at capacity.
	 */
	static void Store(FUnrealOpenMcpCheckpointStoreEntry Entry);

	/**
	 * Get an entry by CheckpointId. Bumps the access clock so active
	 * checkpoints survive LRU eviction under pressure. Returns nullptr when
	 * the id is unknown (caller owns the pointer; valid only until the next
	 * store mutation).
	 */
	static const FUnrealOpenMcpCheckpointStoreEntry* Get(const FString& CheckpointId);

	/** Current entry count. */
	static int32 Count();

	/** Wipe every entry (test helper). */
	static void Clear();
};
