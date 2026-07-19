// Bounded GLog ring-buffer collector for the console tool family (P5.3).
//
// A single process-wide FOutputDevice registered with GLog on editor module
// start and unregistered on shutdown. Every engine / UE_LOG line after
// registration is appended to a fixed-capacity ring buffer with a monotonic
// sequence number, a masked verbosity, a category, a truncated message, and a
// capture timestamp. The console_get_logs / console_clear_logs tools read and
// empty this buffer.
//
// This is NOT a guaranteed full Output Log mirror:
//   - Cold-start gap: lines logged BEFORE the plugin registers the collector
//     are never captured (the plugin's own boot line is the earliest possible
//     entry). Documented in the tool description + docs/api.md.
//   - Ring overflow: when the buffer is full the OLDEST entry is overwritten,
//     so the buffer is a recent-N window, not the whole session.
//   - Per-entry message truncation: a single very long line is clipped to
//     MaxMessageLen so one log line can never flood a tool payload.
//
// Relationship to the per-call `logs[]` window: this ring is a SESSION-scoped
// sink fed by the single GLog callback; the per-call `logs[]` some mutating
// tools return is a CALL-scoped capture. There is exactly one GLog device (this
// one) — the console family does not add a second collector.
//
// Thread-safety: GLog invokes output devices from ANY thread, so Serialize
// takes a lock and the collector advertises CanBeUsedOnAnyThread. The read /
// clear methods lock the same critical section. The tool handlers themselves
// run on the game thread (marshaled by the dispatcher), but they still lock
// because a background thread may be mid-Serialize.
#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

/**
 * Process-wide bounded log ring buffer. Access the singleton via Get(); the
 * editor module owns its GLog registration lifecycle via Start() / Stop().
 */
class UNREALOPENMCPEDITOR_API FUnrealOpenMcpLogCollector : public FOutputDevice
{
public:
	/** Default ring capacity. Mirrors Unreal-MCP's 10000-entry collector. */
	static constexpr int32 DefaultMaxEntries = 10000;
	/** Per-entry message cap (characters). A longer line is clipped + flagged. */
	static constexpr int32 MaxMessageLen = 2000;

	/** One captured log line. */
	struct FEntry
	{
		uint64 Sequence = 0;
		ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
		FName Category;
		FString Message;
		FDateTime Timestamp;
	};

	/** Filter applied by Snapshot. All fields optional. */
	struct FFilter
	{
		/** Minimum severity (inclusive). Entries with verbosity numerically <=
		 *  MinVerbosity are kept (Fatal=1 … VeryVerbose=7 — lower is more
		 *  severe). ELogVerbosity::All (0-mask sentinel) keeps everything. */
		ELogVerbosity::Type MinVerbosity = ELogVerbosity::All;
		/** Category exact match (case-insensitive) when non-None. */
		FName Category;
		/** Case-insensitive substring the message must contain when non-empty. */
		FString Contains;
		/** Max entries returned (the most recent matches). */
		int32 Limit = 200;
	};

	/** The process-wide instance. */
	static FUnrealOpenMcpLogCollector& Get();

	/** Register with GLog. Idempotent. Allocates the ring on first start. */
	void Start();
	/** Unregister from GLog. Idempotent. Safe to call during module shutdown. */
	void Stop();

	// FOutputDevice.
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	/**
	 * Return the most recent entries matching @p Filter, oldest→newest within
	 * the returned window. Sets @p OutMatched to the total number of entries
	 * that matched the filter (before the limit) so the caller can report a
	 * truncated flag (matched > returned).
	 */
	TArray<FEntry> Snapshot(const FFilter& Filter, int32& OutMatched) const;

	/** Empty the buffer. Returns the number of entries removed. The sequence
	 *  counter stays monotonic across a clear. */
	int32 Clear();

	/** Current number of buffered entries (for tests / diagnostics). */
	int32 Num() const;

	/** Parse a verbosity token ("error"/"warning"/…) into ELogVerbosity.
	 *  Returns ELogVerbosity::All for an empty/"all" token; NumVerbosity (an
	 *  out-of-range sentinel) when the token is not recognized. */
	static ELogVerbosity::Type ParseVerbosity(const FString& Token);
	/** Render a verbosity as a stable lowercase-free token
	 *  ("Fatal"/"Error"/"Warning"/"Display"/"Log"/"Verbose"/"VeryVerbose"). */
	static FString VerbosityToString(ELogVerbosity::Type Verbosity);

private:
	FUnrealOpenMcpLogCollector() = default;

	mutable FCriticalSection Lock;
	/** Fixed-capacity ring. Sized to DefaultMaxEntries on Start. */
	TArray<FEntry> Buffer;
	int32 Head = 0;   // index of the oldest entry
	int32 Count = 0;  // number of live entries
	uint64 NextSequence = 1;
	bool bRegistered = false;
};
