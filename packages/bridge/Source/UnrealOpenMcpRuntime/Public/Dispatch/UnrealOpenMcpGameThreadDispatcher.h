// Game-thread dispatcher — the single marshaling path for all UObject / Editor
// API access in the bridge.
//
// Ports Unity Open MCP's MainThreadDispatcher
// (packages/bridge/Editor/Bridge/MainThreadDispatcher.cs) to Unreal with the
// IsInGameThread() short-circuit + AsyncTask(GameThread) pump pattern adapted
// from the Unreal-MCP behavior reference (Dispatch/UnrealMcpGameThreadDispatcher).
// Unity's contract is preserved: a fire-and-forget Enqueue plus a typed
// EnqueueAsync<T> request/response flow with a per-call timeout that
// distinguishes "the game thread never picked the work up" (a modal / long
// editor op is blocking it) from "the work started but ran past the timeout".
//
// Why Runtime placement: the dispatcher is packaging-safe infrastructure with
// no editor-only hooks. It lives here so a future packaged-game commandlet
// path can reuse it; the Editor module owns only lifecycle (start/stop). This
// keeps the Editor→Runtime boundary invariant
// (packages/bridge/AGENTS.md §Transport; docs/architecture.md) one-directional.
//
// Threading contract:
//   - Handlers ALWAYS run on the game thread. Callers may invoke Enqueue /
//     EnqueueAsync from any thread (typically the HTTP listener worker).
//   - If the caller is already on the game thread, the body runs INLINE before
//     the call returns (Unreal-MCP pattern) — pinned in the specs. This avoids
//     a reentrant queue pump and keeps single-threaded test paths simple.
//   - Never Wait() on a future from the game thread for work that itself must
//     run on the game thread — that deadlocks. The HTTP layer (P1.3) waits on
//     its listener worker thread, never on the game thread.
#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"

#include <atomic>        // for std::atomic<bool> in TDispatchState
#include <type_traits>   // for std::is_void_v in RunBody

/**
 * Outcome categories for a dispatched call.
 *
 * Stable string codes the P1.3 HTTP layer maps to error envelopes — aligned
 * with Unity's MainThreadBlockedException vs TimeoutException split so the
 * agent-facing error surface is parity-compatible.
 */
enum class EUnrealOpenMcpDispatchResult : uint8
{
	/** The body ran to completion and returned a value. */
	Success,
	/** The body threw (returned a future in the faulted state). */
	Faulted,
	/** The game thread never started draining the call within the timeout — a
	 * modal dialog or long editor operation is almost certainly blocking it. */
	GameThreadBlocked,
	/** The body started running but did not finish within the timeout. */
	Timeout,
	/** The dispatcher was torn down before/while the call was pending. Pending
	 * waiters fail immediately with this code instead of burning the full
	 * timeout (post-teardown fail-fast, see Shutdown()). */
	DispatcherShutdown,
};

/**
 * A typed result for EnqueueAsync<T>. Carries the outcome category plus either
 * the body's value (Success) or a structured error code/message the HTTP layer
 * maps to an envelope. Default-constructible so test specs can build expected
 * values without ceremony.
 */
template <typename T>
struct TUnrealOpenMcpDispatchResult
{
	EUnrealOpenMcpDispatchResult Result = EUnrealOpenMcpDispatchResult::Faulted;
	FString Code;        // empty for Success
	FString Message;     // empty for Success
	TOptional<T> Value;  // set only for Success

	static TUnrealOpenMcpDispatchResult Ok(T InValue)
	{
		TUnrealOpenMcpDispatchResult Out;
		Out.Result = EUnrealOpenMcpDispatchResult::Success;
		Out.Value = MoveTemp(InValue);
		return Out;
	}

	static TUnrealOpenMcpDispatchResult MakeError(
		EUnrealOpenMcpDispatchResult InResult, FString InCode, FString InMessage)
	{
		TUnrealOpenMcpDispatchResult Out;
		Out.Result = InResult;
		Out.Code = MoveTemp(InCode);
		Out.Message = MoveTemp(InMessage);
		return Out;
	}
};

/**
 * void specialization — for tool bodies that return no value (ack-only). Drops
 * the Value field; Success carries no payload. Needed because
 * TOptional<void> is not a valid type. The HTTP layer maps a successful void
 * result to an empty/ack envelope.
 */
template <>
struct TUnrealOpenMcpDispatchResult<void>
{
	EUnrealOpenMcpDispatchResult Result = EUnrealOpenMcpDispatchResult::Faulted;
	FString Code;
	FString Message;

	static TUnrealOpenMcpDispatchResult Ok()
	{
		TUnrealOpenMcpDispatchResult Out;
		Out.Result = EUnrealOpenMcpDispatchResult::Success;
		return Out;
	}

	static TUnrealOpenMcpDispatchResult MakeError(
		EUnrealOpenMcpDispatchResult InResult, FString InCode, FString InMessage)
	{
		TUnrealOpenMcpDispatchResult Out;
		Out.Result = InResult;
		Out.Code = MoveTemp(InCode);
		Out.Message = MoveTemp(InMessage);
		return Out;
	}
};

/**
 * The game-thread dispatcher. Owned (start/stop) by FUnrealOpenMcpEditorModule;
 * used by the HTTP server (P1.3) and every tool handler that touches UObjects.
 *
 * One instance per editor process. Not a singleton — ownership flows from the
 * Editor module so the lifecycle is explicit and testable.
 */
class UNREALOPENMCPRUNTIME_API FUnrealOpenMcpGameThreadDispatcher
{
public:
	FUnrealOpenMcpGameThreadDispatcher() = default;
	~FUnrealOpenMcpGameThreadDispatcher();

	/**
	 * Enqueue a fire-and-forget action on the game thread. Runs inline when the
	 * caller is already on the game thread. No result, no timeout — the caller
	 * cannot observe failure. Mirrors Unity's MainThreadDispatcher.Enqueue.
	 *
	 * No-op (returns immediately) after Shutdown() — a torn-down dispatcher
	 * silently drops fire-and-forget work; only EnqueueAsync waiters are told.
	 */
	void Enqueue(TFunction<void()> Action);

	/**
	 * Enqueue a typed body on the game thread and return a future for its
	 * result. Mirrors Unity's MainThreadDispatcher.EnqueueAsync<T>.
	 *
	 * @param Body       runs on the game thread (inline if already on it).
	 * @param TimeoutMs  per-call timeout. When it elapses the result
	 *                   distinguishes GameThreadBlocked (the body never started
	 *                   draining — a modal is likely blocking the game thread)
	 *                   from Timeout (the body started but ran long). Late body
	 *                   completion is dropped (single-completion guard).
	 * @return A future that resolves to a TUnrealOpenMcpDispatchResult<T>. If
	 *         the dispatcher is already shut down, resolves immediately to a
	 *         DispatcherShutdown error (no timeout burn).
	 */
	template <typename T>
	TFuture<TUnrealOpenMcpDispatchResult<T>> EnqueueAsync(
		TFunction<T()> Body, uint32 TimeoutMs);

	/**
	 * Tear down the dispatcher. After this returns:
	 *   - Enqueue is a silent no-op.
	 *   - EnqueueAsync resolves immediately with DispatcherShutdown.
	 *   - In-flight waiters are completed with DispatcherShutdown (bounded
	 *     drain — never burn the full per-call timeout during editor teardown).
	 *
	 * Does NOT block on still-running game-thread bodies: those are owned by
	 * the game thread's own teardown and will complete/fault through their
	 * promises' single-completion guard without the dispatcher's involvement.
	 * Safe to call from the game thread during ShutdownModule.
	 */
	void Shutdown();

	/** True after Shutdown() has been called. Read-only diagnostic. */
	bool IsShutdown() const { return bShutdown; }

private:
	/** Drain-stamp bookkeeping shared between the game-thread body and the
	 * timeout watcher. The watcher reads bStartedDrain to split
	 * GameThreadBlocked (false → never picked up) from Timeout (true → ran
	 * long). Mirrors Unity's QueuedAction.StartedDrainAtUtc. */
	struct FDispatchStateBase
	{
		/** Set to true the instant the game thread begins running the body.
		 *  Written on the game thread, read by the timeout watcher thread. */
		FThreadSafeBool bStartedDrain;
	};

	/**
	 * Shared, thread-safe completion guard for one dispatched call. The first
	 * path to win the atomic compare-exchange completes the promise; every
	 * later path (late body, late timeout, teardown) is a no-op. This is the
	 * single-completion invariant — the load-bearing safety property.
	 */
	template <typename T>
	struct TDispatchState : FDispatchStateBase
	{
		TPromise<TUnrealOpenMcpDispatchResult<T>> Promise;
		std::atomic<bool> bCompleted{false};

		void Complete(TUnrealOpenMcpDispatchResult<T>&& InResult)
		{
			bool bExpected = false;
			if (bCompleted.compare_exchange_strong(bExpected, true))
			{
				Promise.SetValue(MoveTemp(InResult));
			}
		}
	};

	/** Pooled manual-reset event shared (ref-counted) between the game-thread
	 *  body and the timeout watcher. Whichever finishes LAST returns it to the
	 *  pool, so a slow body that triggers the event after the watcher already
	 *  gave up can never poke a recycled event. Pattern from the
	 *  Unreal-MCP reference (FSharedPooledEvent). */
	struct FSharedPooledEvent
	{
		FEvent* Event = nullptr;
		FSharedPooledEvent();
		~FSharedPooledEvent();
	};

	/**
	 * Resolve an EnqueueAsync dispatch after the body has either returned a
	 * value or thrown. Runs on the game thread. Marks the drain start, then
	 * completes the shared state. Late resolution (after a timeout already
	 * completed the promise) is absorbed by the single-completion guard.
	 */
	template <typename T>
	static void RunBody(
		const TSharedRef<TDispatchState<T>, ESPMode::ThreadSafe>& State,
		const TSharedRef<FSharedPooledEvent, ESPMode::ThreadSafe>& Done,
		TFunction<T()>&& Body);

	/** True once Shutdown() has run. Reads are benign and happen off the game
	 *  thread (HTTP worker); writes are game-thread only in Shutdown(). */
	FThreadSafeBool bShutdown;
};

// ---------------------------------------------------------------------------
// Template implementation — must be visible to every translation unit that
// instantiates EnqueueAsync<T>. Kept in the header (no extern template
// specialization registry is worth the complexity at this stage).
// ---------------------------------------------------------------------------

template <typename T>
void FUnrealOpenMcpGameThreadDispatcher::RunBody(
	const TSharedRef<TDispatchState<T>, ESPMode::ThreadSafe>& State,
	const TSharedRef<FSharedPooledEvent, ESPMode::ThreadSafe>& Done,
	TFunction<T()>&& Body)
{
	// Stamp drain start BEFORE running the body so the timeout watcher can
	// split "never started" (GameThreadBlocked) from "ran long" (Timeout).
	State->bStartedDrain = true;

	TUnrealOpenMcpDispatchResult<T> Resolved;
	try
	{
		// void vs non-void split: void bodies cannot return a value to Ok(),
		// so invoke the body as a statement and call the zero-arg Ok(). The
		// if-constexpr form keeps a single template for both cases.
		if constexpr (std::is_void_v<T>)
		{
			Body();
			Resolved = TUnrealOpenMcpDispatchResult<T>::Ok();
		}
		else
		{
			Resolved = TUnrealOpenMcpDispatchResult<T>::Ok(Body());
		}
	}
	catch (...)
	{
		// C++ exceptions are not the primary error channel in Unreal, but the
		// body is arbitrary caller code; keep the dispatcher crash-safe. The
		// HTTP layer maps Faulted to a generic execution_error envelope.
		Resolved = TUnrealOpenMcpDispatchResult<T>::MakeError(
			EUnrealOpenMcpDispatchResult::Faulted,
			TEXT("body_faulted"),
			TEXT("Dispatched body threw an exception."));
	}

	State->Complete(MoveTemp(Resolved));
	Done->Event->Trigger(); // release the timeout watcher if it is still waiting
}

template <typename T>
TFuture<TUnrealOpenMcpDispatchResult<T>> FUnrealOpenMcpGameThreadDispatcher::EnqueueAsync(
	TFunction<T()> Body, uint32 TimeoutMs)
{
	using FState = TDispatchState<T>;
	using FStateRef = TSharedRef<FState, ESPMode::ThreadSafe>;
	using FEventRef = TSharedRef<FSharedPooledEvent, ESPMode::ThreadSafe>;

	const FStateRef State = MakeShared<FState, ESPMode::ThreadSafe>();
	const FEventRef Done = MakeShared<FSharedPooledEvent, ESPMode::ThreadSafe>();
	TFuture<TUnrealOpenMcpDispatchResult<T>> Future = State->Promise.GetFuture();

	// Post-teardown fail-fast: resolve immediately, do not burn the timeout.
	if (bShutdown)
	{
		State->Complete(TUnrealOpenMcpDispatchResult<T>::MakeError(
			EUnrealOpenMcpDispatchResult::DispatcherShutdown,
			TEXT("dispatcher_shutdown"),
			TEXT("Game-thread dispatcher is shutting down.")));
		return Future;
	}

	// RunBody closes over State/Done and performs the drain stamp + completion.
	auto RunBodyLambda = [State, Done, Body = MoveTemp(Body)]() mutable
	{
		RunBody<T>(State, Done, MoveTemp(Body));
	};

	if (IsInGameThread())
	{
		// Inline short-circuit — pinned in the specs. Avoids a reentrant pump
		// and keeps single-threaded test paths deterministic.
		RunBodyLambda();
	}
	else
	{
		AsyncTask(ENamedThreads::GameThread, MoveTemp(RunBodyLambda));
	}

	// Always arm the timeout watcher on a pool thread. When the body already
	// finished (inline or fast), the event is already triggered and Wait()
	// returns immediately. Both lambdas hold a ref to the pooled event so it
	// is returned only after BOTH finish — never while a still-pending body
	// might trigger it.
	const uint32 ClampedTimeoutMs = FMath::Max<uint32>(1u, TimeoutMs);
	Async(EAsyncExecution::ThreadPool, [State, Done, ClampedTimeoutMs]()
	{
		// 0 → timed out (event never triggered within the window).
		if (Done->Event->Wait(ClampedTimeoutMs))
		{
			return; // body finished first and already completed the promise
		}

		// Timed out. Split GameThreadBlocked (never drained — a modal is likely
		// blocking the game thread) from Timeout (started but ran long). Aligns
		// with Unity's MainThreadBlockedException vs TimeoutException.
		if (!State->bStartedDrain)
		{
			State->Complete(TUnrealOpenMcpDispatchResult<T>::MakeError(
				EUnrealOpenMcpDispatchResult::GameThreadBlocked,
				TEXT("game_thread_blocked"),
				TEXT("The Unreal game thread did not process the dispatch within the timeout — ")
				TEXT("a modal dialog (unsaved changes, scene modified externally) or a long ")
				TEXT("editor operation is almost certainly blocking it.")));
		}
		else
		{
			State->Complete(TUnrealOpenMcpDispatchResult<T>::MakeError(
				EUnrealOpenMcpDispatchResult::Timeout,
				TEXT("timeout"),
				TEXT("The dispatched body did not finish within the per-call timeout.")));
		}
	});

	return Future;
}
