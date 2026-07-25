// Non-template implementation for FUnrealOpenMcpGameThreadDispatcher.
// EnqueueAsync<T> lives entirely in the header (template); this translation
// unit owns the destructor, fire-and-forget Enqueue, Shutdown, and the pooled
// event helpers.
#include "Dispatch/UnrealOpenMcpGameThreadDispatcher.h"
#include "UnrealOpenMcpLog.h"

#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"

FUnrealOpenMcpGameThreadDispatcher::FSharedPooledEvent::FSharedPooledEvent()
{
	// Manual-reset so the timeout watcher's Wait() stays signaled once the
	// game-thread body triggers it — and so a late trigger after the watcher
	// already gave up cannot be missed by a subsequent (recycled) waiter.
	Event = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ true);
}

FUnrealOpenMcpGameThreadDispatcher::FSharedPooledEvent::~FSharedPooledEvent()
{
	if (Event != nullptr)
	{
		FPlatformProcess::ReturnSynchEventToPool(Event);
		Event = nullptr;
	}
}

FUnrealOpenMcpGameThreadDispatcher::~FUnrealOpenMcpGameThreadDispatcher()
{
	// Defensive Shutdown() if the owner forgot — module destruct order can
	// race the Editor module's explicit ShutdownModule call. Idempotent.
	Shutdown();
}

void FUnrealOpenMcpGameThreadDispatcher::Enqueue(TFunction<void()> Action)
{
	// Silent no-op after teardown. Fire-and-forget callers cannot observe
	// failure; only EnqueueAsync waiters are told (DispatcherShutdown). This
	// keeps teardown quiet — a late Enqueue during editor quit must not crash.
	if (bShutdown)
	{
		return;
	}

	if (IsInGameThread())
	{
		Action();
	}
	else
	{
		AsyncTask(ENamedThreads::GameThread, MoveTemp(Action));
	}
}

bool FUnrealOpenMcpGameThreadDispatcher::TrackInFlight(
	const TWeakPtr<FDispatchStateBase, ESPMode::ThreadSafe>& State)
{
	FScopeLock Lock(&InFlightLock);

	// Shutdown test and insertion under the SAME lock — see the header for why
	// splitting them reintroduces the teardown deadlock.
	if (bShutdown)
	{
		return false;
	}

	// Prune completed dispatches so the list is bounded by concurrency, not by
	// the number of calls the process has ever served.
	InFlight.RemoveAllSwap(
		[](const TWeakPtr<FDispatchStateBase, ESPMode::ThreadSafe>& Entry)
		{
			return !Entry.IsValid();
		});

	InFlight.Add(State);
	return true;
}

void FUnrealOpenMcpGameThreadDispatcher::Shutdown()
{
	// Idempotent — module reload / editor teardown may call this more than once.
	//
	// The flag is flipped under InFlightLock so it is atomic with respect to
	// TrackInFlight: a dispatch either registers before the sweep (and gets
	// completed by it) or is refused registration and fails fast. There is no
	// window where one slips in unregistered.
	{
		FScopeLock Lock(&InFlightLock);
		if (bShutdown)
		{
			return;
		}
		bShutdown = true;
	}

	// After the flag flips, new EnqueueAsync calls resolve immediately with
	// DispatcherShutdown (fail-fast) and new Enqueue calls are silent no-ops.
	//
	// In-flight waiters must be completed EAGERLY here, not left to their
	// timeout watchers. A listener thread parked in Future.Get() is waiting on a
	// body queued onto the game thread; during teardown the game thread is the
	// one running ShutdownModule, so that body may never run. Leaving the waiter
	// pending means the Editor module's subsequent StopAndJoin() blocks the game
	// thread on a worker that is blocked on the game thread — a circular wait
	// held for up to the caller-supplied timeout (clamped at 600 s). Resolving
	// them now breaks the cycle and makes the join prompt.
	//
	// The single-completion guard absorbs the race: if a body or timeout watcher
	// already resolved a state, CompleteWithDispatcherShutdown is a no-op.
	// Snapshot under the lock, then complete OUTSIDE it — a completion can run
	// arbitrary continuation code and must not re-enter TrackInFlight holding
	// the same lock.
	TArray<TSharedPtr<FDispatchStateBase, ESPMode::ThreadSafe>> Pending;
	{
		FScopeLock Lock(&InFlightLock);
		Pending.Reserve(InFlight.Num());
		for (const TWeakPtr<FDispatchStateBase, ESPMode::ThreadSafe>& Entry : InFlight)
		{
			if (TSharedPtr<FDispatchStateBase, ESPMode::ThreadSafe> Pinned = Entry.Pin())
			{
				Pending.Add(MoveTemp(Pinned));
			}
		}
		InFlight.Reset();
	}

	for (const TSharedPtr<FDispatchStateBase, ESPMode::ThreadSafe>& State : Pending)
	{
		State->CompleteWithDispatcherShutdown();

		// Also WAKE the timeout watcher. Completing the promise unblocks the
		// listener thread, but the watcher is parked in Event->Wait(TimeoutMs) and
		// resolving the promise does not signal that event. Leaving it parked just
		// relocates the teardown stall to GThreadPool, which
		// FQueuedThreadPool::Destroy() waits on at engine exit. The event is
		// manual-reset and ref-counted, so triggering it here is safe even if the
		// body already did.
		if (TSharedPtr<FSharedPooledEvent, ESPMode::ThreadSafe> Event = State->WatcherEvent.Pin())
		{
			if (Event->Event != nullptr)
			{
				Event->Event->Trigger();
			}
		}
	}

	// The load-bearing property is: Shutdown never blocks on the game thread
	// for game-thread work (that would deadlock), and never crashes.
	UE_LOG(
		LogUnrealOpenMcp,
		Log,
		TEXT("[Unreal Open MCP] game-thread dispatcher shut down (%d pending dispatch(es) failed fast)"),
		Pending.Num());
}
