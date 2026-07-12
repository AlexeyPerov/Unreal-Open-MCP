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

void FUnrealOpenMcpGameThreadDispatcher::Shutdown()
{
	// Idempotent — module reload / editor teardown may call this more than once.
	if (bShutdown)
	{
		return;
	}
	bShutdown = true;

	// After the flag flips, new EnqueueAsync calls resolve immediately with
	// DispatcherShutdown (fail-fast), and new Enqueue calls are silent
	// no-ops. In-flight waiters whose bodies are still queued on the game
	// thread will not get to run during editor teardown — their timeout
	// watchers would eventually fire, but we do not want to burn the full
	// per-call timeout during shutdown. There is no global registry of pending
	// states to complete them eagerly here because the state objects are
	// owned per-call by the shared refs captured into the body + watcher
	// lambdas; tearing those down eagerly would require a registry, which is
	// deferred until a real deadlock is observed. The single-completion guard
	// means whichever path resolves them first (late body via game-thread
	// teardown, or the timeout watcher) wins, and the other is a no-op.
	//
	// The load-bearing property is: Shutdown never blocks on the game thread
	// for game-thread work (that would deadlock), and never crashes.
	UE_LOG(
		LogUnrealOpenMcp,
		Log,
		TEXT("[Unreal Open MCP] game-thread dispatcher shut down (pending dispatches fail-fast)"));
}
