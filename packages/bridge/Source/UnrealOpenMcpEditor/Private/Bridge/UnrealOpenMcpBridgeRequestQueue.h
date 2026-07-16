// Fair request queue for tool dispatch (deferred from P1.2).
//
// Adapts Unity Open MCP's BridgeRequestQueue
// (packages/bridge/Editor/Bridge/BridgeRequestQueue.cs — per-agent FIFO +
// round-robin across agents keyed on the X-Agent-Id header) to Unreal C++.
//
// Why a queue when the P2.1 listener is single-connection? The accept loop
// serves one connection at a time today, so dispatch is already serialized.
// The queue is introduced now (rather than deferred again) so that:
//   1. The X-Agent-Id bookkeeping is in place the moment a second concurrent
//      dispatch path appears (a future listener fan-out / keep-alive), with no
//      contract change for tool handlers.
//   2. Fairness is testable in isolation — the spec pins per-agent FIFO +
//      round-robin across agents without needing a concurrent listener.
//   3. Starvation protection: a single agent issuing many requests cannot
//      starve a second agent when concurrency expands.
//
// Fairness contract (mirrors Unity's BridgeRequestScheduler.PickForFrame):
//   - Each agent id gets its own FIFO queue.
//   - Across agents, dequeue is round-robin by agent arrival order, advancing
//     a cursor so no agent is starved.
//   - Anonymous callers (no X-Agent-Id header) share one synthetic agent id so
//     hand-rolled curl traffic still flows.
//
// P2.1 usage: the HTTP server calls Submit(AgentId, Work) per dispatch; Work
// runs synchronously on the caller's thread inside Submit (the listener
// worker) today. The fairness state is maintained so the structure is correct
// when Work moves to the game-thread tick path. This is the honest P2.1
// shape — the queue is wired and fair, the concurrency is single-stream.
#pragma once

#include "CoreMinimal.h"
#include "Bridge/UnrealOpenMcpToolRegistry.h"

/**
 * A unit of queued work. Carries the agent id that submitted it (for the
 * per-agent FIFO invariant) and the tool name (for diagnostics / mutating-tool
 * gating in later phases). The Work lambda returns the dispatch result.
 */
struct FUnrealOpenMcpQueuedRequest
{
	FString AgentId;
	FString ToolName;
	/** Runs the tool handler and returns its dispatch result. */
	TFunction<FUnrealOpenMcpToolDispatchResult()> Work;
};

/**
 * Fair round-robin request queue keyed by agent id.
 *
 * Thread-safe. The HTTP listener worker calls Submit; today Submit runs the
 * work inline (single-stream listener), but the queue records the arrival so
 * fairness accounting is exercised and testable.
 */
class UNREALOPENMCPEDITOR_API FUnrealOpenMcpBridgeRequestQueue
{
public:
	FUnrealOpenMcpBridgeRequestQueue() = default;
	~FUnrealOpenMcpBridgeRequestQueue() = default;

	/** Not copyable — holds queued work. */
	FUnrealOpenMcpBridgeRequestQueue(const FUnrealOpenMcpBridgeRequestQueue&) = delete;
	FUnrealOpenMcpBridgeRequestQueue& operator=(const FUnrealOpenMcpBridgeRequestQueue&) = delete;

	/**
	 * Submit a request and run it through the fair path.
	 *
	 * In P2.1 the listener is single-stream, so Submit dequeues + runs the
	 * work inline on the caller's thread and returns the result. The agent-id
	 * bookkeeping (per-agent FIFO ordering, round-robin cursor advancement) is
	 * maintained so that when a concurrent dispatch path lands, fairness is
	 * already correct.
	 *
	 * @param AgentId   the X-Agent-Id header value (or a synthetic id).
	 * @param ToolName  the tool being dispatched (diagnostics / future mutating gating).
	 * @param Work      the tool handler invocation.
	 * @return the dispatch result from Work.
	 */
	FUnrealOpenMcpToolDispatchResult Submit(
		const FString& AgentId,
		const FString& ToolName,
		TFunction<FUnrealOpenMcpToolDispatchResult()> Work);

	/**
	 * Pick the next request to run, fairly across agents. Public so the spec
	 * can pin the round-robin order without submitting real work. Does NOT run
	 * the work — returns the request (with AgentId + ToolName) and removes it
	 * from the queue.
	 *
	 * Returns false when the queue is empty. Used by Submit internally and by
	 * the fairness spec directly.
	 */
	bool PickNext(FUnrealOpenMcpQueuedRequest& OutRequest);

	/**
	 * Enqueue a request WITHOUT picking/running it. Public for the fairness
	 * spec — production paths use Submit (enqueue + pick + run in one call).
	 * Returns the queue depth for this agent after the enqueue.
	 */
	int32 Enqueue(const FString& AgentId, const FString& ToolName, TFunction<FUnrealOpenMcpToolDispatchResult()> Work);

	/** Number of pending requests across all agents. */
	int32 PendingCount() const;

	/** Number of distinct agents with pending work. */
	int32 AgentCount() const;

private:
	/** Agent arrival order — drives the round-robin cursor. */
	TArray<FString> AgentOrder;
	/** Agent id → that agent's FIFO queue. */
	TMap<FString, TArray<FUnrealOpenMcpQueuedRequest>> AgentQueues;
	/** Round-robin cursor into AgentOrder. */
	int32 RRCursor = 0;
	mutable FCriticalSection Lock;

	/** Remove agents whose queues are empty, clamping the cursor. Called under
	 *  Lock by PickNext. */
	void CompactAgents();
};
