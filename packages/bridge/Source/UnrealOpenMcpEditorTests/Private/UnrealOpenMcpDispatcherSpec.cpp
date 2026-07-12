// Game-thread dispatcher Automation specs.
//
// Ports Unity Open MCP's MainThreadDispatcher contract (Enqueue /
// EnqueueAsync + the GameThreadBlocked-vs-Timeout split from
// specs/feedback.md 2026-07-03) and adapts the Unreal-MCP reference spec
// (UnrealMcpDispatcherSpec.cpp — IsInGameThread short-circuit + AsyncTask
// marshal + timeout + single-completion guard patterns).
//
// Contract pinned here (P1.2 acceptance criteria):
//   - Off-game-thread calls marshal to the game thread reliably.
//   - Already-on-game-thread calls run inline (Unreal-MCP pattern).
//   - FIFO order holds for off-thread EnqueueAsync dispatches.
//   - Per-call timeout distinguishes GameThreadBlocked (never drained) from
//     Timeout (ran long).
//   - Shutdown does not deadlock; post-teardown EnqueueAsync resolves
//     immediately with DispatcherShutdown (no full-timeout burn).
//   - Single-completion guard: a late body never overwrites the timeout
//     result.
//
// These specs are pure dispatcher tests — no editor assets, no HTTP. They run
// in the editor automation runner (the game thread is pumped between latent
// steps so marshalled AsyncTasks get a chance to land).
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dispatch/UnrealOpenMcpGameThreadDispatcher.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpDispatcherSpec,
	"UnrealOpenMcp.Dispatch.GameThread",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpDispatcherSpec)

void FUnrealOpenMcpDispatcherSpec::Define()
{
	Describe("EnqueueAsync", [this]()
	{
		// Inline short-circuit: when the caller is already on the game thread
		// (the automation runner's home thread), the body runs BEFORE the
		// EnqueueAsync call returns. The future is already resolved.
		It("runs the body inline on the game thread", [this]()
		{
			FUnrealOpenMcpGameThreadDispatcher Dispatcher;
			bool bRanOnGameThread = false;
			bool bBodyRan = false;

			TFuture<TUnrealOpenMcpDispatchResult<int32>> Future = Dispatcher.EnqueueAsync<int32>(
				[&bRanOnGameThread, &bBodyRan]() -> int32
				{
					bBodyRan = true;
					bRanOnGameThread = IsInGameThread();
					return 42;
				},
				/*TimeoutMs*/ 5000);

			// Inline expectation: body already ran before we wait.
			TestTrue(TEXT("body ran inline (before wait)"), bBodyRan);
			TestTrue(TEXT("body ran on game thread"), bRanOnGameThread);

			const TUnrealOpenMcpDispatchResult<int32> Result = Future.Get();
			TestEqual(TEXT("success result"), Result.Result, EUnrealOpenMcpDispatchResult::Success);
			TestTrue(TEXT("value present"), Result.Value.IsSet());
			TestEqual(TEXT("value"), Result.Value.GetValue(), 42);
		});

		// Off-game-thread marshal: dispatched from a background thread, the
		// body must STILL execute on the game thread (the load-bearing DoD
		// claim). Latent because the automation runner pumps the game thread
		// between frames so the marshalled AsyncTask can land.
		LatentIt(
			"marshals an off-thread call onto the game thread",
			FTimespan::FromSeconds(15),
			[this](const FDoneDelegate& Done)
		{
			Async(EAsyncExecution::Thread, [this, Done]()
			{
				FUnrealOpenMcpGameThreadDispatcher Dispatcher;
				const bool bCalledOffGameThread = !IsInGameThread();
				bool bBodyOnGameThread = false;

				TFuture<TUnrealOpenMcpDispatchResult<FString>> Future =
					Dispatcher.EnqueueAsync<FString>(
						[&bBodyOnGameThread]() -> FString
						{
							bBodyOnGameThread = IsInGameThread();
							return TEXT("marshalled");
						},
						/*TimeoutMs*/ 10000);

				const TUnrealOpenMcpDispatchResult<FString> Result = Future.Get();
				TestTrue(TEXT("dispatched off the game thread"), bCalledOffGameThread);
				TestTrue(TEXT("body ran on the game thread"), bBodyOnGameThread);
				TestEqual(TEXT("success result"), Result.Result, EUnrealOpenMcpDispatchResult::Success);
				TestEqual(TEXT("value"), Result.Value.Get(TEXT("")), FString(TEXT("marshalled")));
				Done.Execute();
			});
		});

		// FIFO ordering: two bodies dispatched from a background thread in
		// order must observe their start order on the game thread. The
		// dispatcher must not reorder the AsyncTask queue.
		LatentIt(
			"preserves FIFO order for off-thread dispatches",
			FTimespan::FromSeconds(15),
			[this](const FDoneDelegate& Done)
		{
			Async(EAsyncExecution::Thread, [this, Done]()
			{
				FUnrealOpenMcpGameThreadDispatcher Dispatcher;
				TArray<int32> Order;

				TFuture<TUnrealOpenMcpDispatchResult<void>> F1 =
					Dispatcher.EnqueueAsync<void>([&Order]() { Order.Add(1); }, 10000);
				TFuture<TUnrealOpenMcpDispatchResult<void>> F2 =
					Dispatcher.EnqueueAsync<void>([&Order]() { Order.Add(2); }, 10000);
				TFuture<TUnrealOpenMcpDispatchResult<void>> F3 =
					Dispatcher.EnqueueAsync<void>([&Order]() { Order.Add(3); }, 10000);

				F1.Get();
				F2.Get();
				F3.Get();

				TestEqual(TEXT("three bodies ran"), Order.Num(), 3);
				if (Order.Num() == 3)
				{
					TestEqual(TEXT("FIFO[0]"), Order[0], 1);
					TestEqual(TEXT("FIFO[1]"), Order[1], 2);
					TestEqual(TEXT("FIFO[2]"), Order[2], 3);
				}
				Done.Execute();
			});
		});

		// Timeout split — "ran long": the body started draining but did not
		// finish within the timeout. Result must be Timeout (NOT
		// GameThreadBlocked), because the body DID start.
		LatentIt(
			"surfaces Timeout when the body starts but runs past the timeout",
			FTimespan::FromSeconds(15),
			[this](const FDoneDelegate& Done)
		{
			// Dispatching the slow body FROM THE GAME THREAD would occupy the
			// game thread and stall the automation runner. Dispatching it off
			// the game thread means it runs on the game thread between frames
			// (the runner pumps), so the body STARTS (bStartedDrain = true)
			// then runs past the short timeout.
			Async(EAsyncExecution::Thread, [this, Done]()
			{
				FUnrealOpenMcpGameThreadDispatcher Dispatcher;

				TFuture<TUnrealOpenMcpDispatchResult<int32>> Future =
					Dispatcher.EnqueueAsync<int32>(
						[]() -> int32
						{
							FPlatformProcess::Sleep(0.5f);
							return 7;
						},
						/*TimeoutMs*/ 100);

				const TUnrealOpenMcpDispatchResult<int32> Result = Future.Get();
				TestEqual(
					TEXT("ran-long -> Timeout (not GameThreadBlocked)"),
					Result.Result,
					EUnrealOpenMcpDispatchResult::Timeout);
				TestEqual(TEXT("code"), Result.Code, FString(TEXT("timeout")));
				Done.Execute();
			});
		});

		// Timeout split — "game thread blocked": the body is queued but NEVER
		// starts draining within the timeout. We simulate a blocked game
		// thread by holding it ourselves: a background thread enqueues the
		// body (so it lands on the game thread's AsyncTask queue, not inline)
		// and waits on its future, while the game thread sleeps past the
		// timeout so the queued AsyncTask never runs. Result must be
		// GameThreadBlocked (NOT Timeout), because bStartedDrain stays false.
		//
		// This mirrors the real failure mode the Unity
		// MainThreadBlockedException was added for: a modal dialog holds the
		// game thread and the queued body never gets to run.
		LatentIt(
			"surfaces GameThreadBlocked when the body never starts draining",
			FTimespan::FromSeconds(15),
			[this](const FDoneDelegate& Done)
		{
			// The background thread enqueues, waits, captures the result, then
			// signals completion. The game thread sleeps (blocking the pump)
			// so the body never drains within the 200ms timeout.
			TSharedRef<TOptional<TUnrealOpenMcpDispatchResult<int32>>, ESPMode::ThreadSafe>
				ResultSlot = MakeShared<TOptional<TUnrealOpenMcpDispatchResult<int32>>, ESPMode::ThreadSafe>();
			FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool(true);

			FUnrealOpenMcpGameThreadDispatcher Dispatcher;
			Async(EAsyncExecution::Thread, [ResultSlot, DoneEvent, &Dispatcher]()
			{
				TFuture<TUnrealOpenMcpDispatchResult<int32>> Future =
					Dispatcher.EnqueueAsync<int32>(
						[]() -> int32 { return 7; },
						/*TimeoutMs*/ 200);
				*ResultSlot = Future.Get();
				DoneEvent->Trigger();
			});

			// Hold the game thread busy well past the 200ms timeout. The
			// queued AsyncTask cannot run, so bStartedDrain stays false and
			// the watcher must classify the failure as GameThreadBlocked.
			FPlatformProcess::Sleep(0.5f);

			// The background thread has resolved the future by now (its
			// timeout watcher fired during our sleep). Collect the result.
			DoneEvent->Wait(2000);
			FPlatformProcess::ReturnSynchEventToPool(DoneEvent);

			TestTrue(TEXT("result captured"), ResultSlot->IsSet());
			if (ResultSlot->IsSet())
			{
				const TUnrealOpenMcpDispatchResult<int32>& Result = ResultSlot->GetValue();
				TestEqual(
					TEXT("never drained -> GameThreadBlocked (not Timeout)"),
					Result.Result,
					EUnrealOpenMcpDispatchResult::GameThreadBlocked);
				TestEqual(TEXT("code"), Result.Code, FString(TEXT("game_thread_blocked")));
			}
			Done.Execute();
		});

		// Single-completion guard: when the body finishes AFTER the timeout
		// already delivered the Timeout result, the late body value must NOT
		// overwrite it, and the second completion must be a no-op (no crash).
		LatentIt(
			"keeps the timeout result when the body completes late",
			FTimespan::FromSeconds(15),
			[this](const FDoneDelegate& Done)
		{
			Async(EAsyncExecution::Thread, [this, Done]()
			{
				FUnrealOpenMcpGameThreadDispatcher Dispatcher;

				TFuture<TUnrealOpenMcpDispatchResult<FString>> Future =
					Dispatcher.EnqueueAsync<FString>(
						[]() -> FString
						{
							FPlatformProcess::Sleep(1.0f); // finishes well after the 100ms timeout
							return TEXT("late-body-value");
						},
						/*TimeoutMs*/ 100);

				const TUnrealOpenMcpDispatchResult<FString> Result = Future.Get();
				TestEqual(TEXT("timed out"), Result.Result, EUnrealOpenMcpDispatchResult::Timeout);
				TestNotEqual(
					TEXT("late body did not overwrite the timeout result"),
					Result.Value.Get(TEXT("")),
					FString(TEXT("late-body-value")));

				// Give the late body time to run and attempt a second
				// completion; the guard must absorb it without altering the
				// delivered result or faulting.
				FPlatformProcess::Sleep(1.5f);
				Done.Execute();
			});
		});
	});

	Describe("Shutdown", [this]()
	{
		// Post-teardown fail-fast: after Shutdown(), a NEW EnqueueAsync must
		// resolve immediately with DispatcherShutdown instead of waiting for
		// the full per-call timeout. This is the bounded-drain contract —
		// editor teardown must not stall on dangling waiters.
		It("fails new EnqueueAsync immediately with DispatcherShutdown", [this]()
		{
			FUnrealOpenMcpGameThreadDispatcher Dispatcher;
			Dispatcher.Shutdown();
			TestTrue(TEXT("shutdown flag set"), Dispatcher.IsShutdown());

			const double Before = FPlatformTime::Seconds();
			TFuture<TUnrealOpenMcpDispatchResult<int32>> Future =
				Dispatcher.EnqueueAsync<int32>(
					[]() -> int32 { return 1; },
					/*TimeoutMs*/ 5000);
			const TUnrealOpenMcpDispatchResult<int32> Result = Future.Get();
			const double ElapsedMs = (FPlatformTime::Seconds() - Before) * 1000.0;

			TestEqual(
				TEXT("post-teardown result"),
				Result.Result,
				EUnrealOpenMcpDispatchResult::DispatcherShutdown);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("dispatcher_shutdown")));
			// Fail-fast: must return near-instantly, not burn the 5s timeout.
			TestTrue(TEXT("returned without burning the timeout"), ElapsedMs < 1000.0);
		});

		// Enqueue after teardown is a silent no-op (callers cannot observe
		// failure for fire-and-forget). The body must NOT run.
		It("silently drops Enqueue after teardown (body does not run)", [this]()
		{
			FUnrealOpenMcpGameThreadDispatcher Dispatcher;
			Dispatcher.Shutdown();

			bool bRan = false;
			Dispatcher.Enqueue([&bRan]() { bRan = true; });

			TestFalse(TEXT("post-teardown Enqueue body did not run"), bRan);
		});

		// Idempotent shutdown: calling Shutdown() twice (module reload /
		// editor teardown race) must not crash or double-resolve anything.
		It("is idempotent (second Shutdown does not crash)", [this]()
		{
			FUnrealOpenMcpGameThreadDispatcher Dispatcher;
			Dispatcher.Shutdown();
			Dispatcher.Shutdown(); // must not crash
			TestTrue(TEXT("still shut down"), Dispatcher.IsShutdown());
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
