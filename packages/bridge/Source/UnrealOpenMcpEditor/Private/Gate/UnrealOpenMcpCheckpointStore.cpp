// FUnrealOpenMcpCheckpointStore implementation. See header for the v1 storage
// contract (process-lifetime, in-memory, no persistence across reloads).
//
// Ported from Unity Open MCP packages/bridge/Editor/Gate/CheckpointStore.cs at
// copy fidelity. The store is a function-local static TArray + TMap pair so
// initialization order is well-defined across translation units.
//
// Game-thread-only: the gate flow runs on the game thread, and the meta-tools
// (P3.6) that will read from here are also game-thread dispatches. No lock is
// taken — Unity's static surface has the same single-thread assumption. If a
// future meta-tool ever runs off the game thread, add an FCriticalSection here
// (the store is the only piece of gate state that ever crosses tool calls).
#include "Gate/UnrealOpenMcpCheckpointStore.h"

#include "Misc/DateTime.h"

namespace UnrealOpenMcpCheckpointStoreInternal
{
	// Insertion-ordered entry list (oldest insert first) + id → entry* index.
	// Unity uses a List + Dictionary; the Unreal port uses TArray + TMap. The
	// list is the LRU traversal surface; the map is the O(1) Get lookup.
	struct FStore
	{
		TArray<FUnrealOpenMcpCheckpointStoreEntry> Entries;
		TMap<FString, int32> IndexById;
	};

	FStore& Get()
	{
		static FStore Instance;
		return Instance;
	}
} // namespace UnrealOpenMcpCheckpointStoreInternal

void FUnrealOpenMcpCheckpointStore::Store(FUnrealOpenMcpCheckpointStoreEntry Entry)
{
	namespace Internal = UnrealOpenMcpCheckpointStoreInternal;
	FStore& S = Internal::Get();

	// Collision handling — overwrite a re-submitted CheckpointId so the latest
	// fingerprint wins. Unity item A: previously a duplicate was a silent
	// no-op; now the new entry replaces the old and recency is refreshed so
	// the overwritten entry moves to the tail (most-recent position).
	if (const int32* FoundIdx = S.IndexById.Find(Entry.CheckpointId))
	{
		S.Entries.RemoveAt(*FoundIdx);
		S.IndexById.Remove(Entry.CheckpointId);
		// Rebuild the index — RemoveAt shifted every later entry.
		S.IndexById.Reset();
		for (int32 i = 0; i < S.Entries.Num(); ++i)
		{
			S.IndexById.Add(S.Entries[i].CheckpointId, i);
		}
	}

	if (Entry.LastAccessedUtc.IsEmpty())
	{
		Entry.LastAccessedUtc = Entry.TimestampUtc.IsEmpty()
			? FDateTime::UtcNow().ToIso8601()
			: Entry.TimestampUtc;
	}

	const int32 NewIdx = S.Entries.Add(MoveTemp(Entry));
	S.IndexById.Add(S.Entries[NewIdx].CheckpointId, NewIdx);

	// LRU eviction under capacity pressure. Drop the entry with the oldest
	// LastAccessedUtc (string comparison is valid for ISO-8601 UTC), not
	// blindly the first-inserted one — an active checkpoint an agent is delta-
	// comparing against must survive newer inserts.
	while (S.Entries.Num() > DefaultCapacity)
	{
		int32 OldestIdx = 0;
		for (int32 i = 1; i < S.Entries.Num(); ++i)
		{
			if (S.Entries[i].LastAccessedUtc.Compare(S.Entries[OldestIdx].LastAccessedUtc) < 0)
			{
				OldestIdx = i;
			}
		}
		S.IndexById.Remove(S.Entries[OldestIdx].CheckpointId);
		S.Entries.RemoveAt(OldestIdx);
	}
}

const FUnrealOpenMcpCheckpointStoreEntry* FUnrealOpenMcpCheckpointStore::Get(const FString& CheckpointId)
{
	namespace Internal = UnrealOpenMcpCheckpointStoreInternal;
	FStore& S = Internal::Get();

	if (const int32* FoundIdx = S.IndexById.Find(CheckpointId))
	{
		// Bump the access clock so active checkpoints survive LRU eviction.
		S.Entries[*FoundIdx].LastAccessedUtc = FDateTime::UtcNow().ToIso8601();
		return &S.Entries[*FoundIdx];
	}
	return nullptr;
}

int32 FUnrealOpenMcpCheckpointStore::Count()
{
	namespace Internal = UnrealOpenMcpCheckpointStoreInternal;
	return Internal::Get().Entries.Num();
}

void FUnrealOpenMcpCheckpointStore::Clear()
{
	namespace Internal = UnrealOpenMcpCheckpointStoreInternal;
	FStore& S = Internal::Get();
	S.Entries.Reset();
	S.IndexById.Reset();
}
