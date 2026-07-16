// Fair request queue — see header for the round-robin rationale and the
// honest P2.1 single-stream usage note.
#include "Bridge/UnrealOpenMcpBridgeRequestQueue.h"

int32 FUnrealOpenMcpBridgeRequestQueue::Enqueue(
	const FString& AgentId,
	const FString& ToolName,
	TFunction<FUnrealOpenMcpToolDispatchResult()> Work)
{
	FScopeLock ScopeLock(&Lock);

	FUnrealOpenMcpQueuedRequest Request;
	Request.AgentId = AgentId;
	Request.ToolName = ToolName;
	Request.Work = MoveTemp(Work);

	// First time we see this agent → append to the arrival-order list so the
	// round-robin cursor visits it. Existing agents keep their position (FIFO
	// across arrivals, not re-appended on each request).
	if (!AgentQueues.Contains(AgentId))
	{
		AgentOrder.Add(AgentId);
		AgentQueues.Add(AgentId, TArray<FUnrealOpenMcpQueuedRequest>());
	}

	TArray<FUnrealOpenMcpQueuedRequest>& Queue = AgentQueues[AgentId];
	Queue.Add(MoveTemp(Request));
	return Queue.Num();
}

bool FUnrealOpenMcpBridgeRequestQueue::PickNext(FUnrealOpenMcpQueuedRequest& OutRequest)
{
	FScopeLock ScopeLock(&Lock);

	if (AgentOrder.Num() == 0)
	{
		return false;
	}

	// Round-robin across agents: starting at the cursor, find the next agent
	// with a non-empty queue. Advance the cursor as we go so no agent is
	// starved — each agent gets a turn before any agent gets a second.
	const int32 StartCursor = RRCursor;
	for (int32 Step = 0; Step < AgentOrder.Num(); ++Step)
	{
		const int32 Idx = (StartCursor + Step) % AgentOrder.Num();
		const FString& AgentId = AgentOrder[Idx];
		TArray<FUnrealOpenMcpQueuedRequest>* Queue = AgentQueues.Find(AgentId);
		if (Queue != nullptr && Queue->Num() > 0)
		{
			OutRequest = MoveTemp(Queue->GetData()[0]);
			Queue->RemoveAt(0, 1, false);
			// Advance the cursor PAST this agent so the next pick starts at the
			// following agent (round-robin, not stick-to-one).
			RRCursor = (Idx + 1) % AgentOrder.Num();
			CompactAgents();
			return true;
		}
	}

	// Every agent's queue is empty (shouldn't happen if PendingCount > 0, but
	// guard against it).
	return false;
}

FUnrealOpenMcpToolDispatchResult FUnrealOpenMcpBridgeRequestQueue::Submit(
	const FString& AgentId,
	const FString& ToolName,
	TFunction<FUnrealOpenMcpToolDispatchResult()> Work)
{
	// Enqueue, then immediately pick + run. In P2.1 the listener is single-
	// stream, so the just-enqueued request is the only pending one and PickNext
	// returns it right back — the fairness accounting (AgentOrder, RRCursor) is
	// exercised but there is no contention. When a concurrent dispatch path
	// lands, PickNext will interleave across agents without this code changing.
	Enqueue(AgentId, ToolName, MoveTemp(Work));

	FUnrealOpenMcpQueuedRequest Request;
	if (!PickNext(Request))
	{
		// Defensive — should be unreachable since we just enqueued.
		return FUnrealOpenMcpToolDispatchResult::Fail(
			TEXT("queue_empty"),
			TEXT("Request queue was empty immediately after enqueue."));
	}

	if (!Request.Work)
	{
		return FUnrealOpenMcpToolDispatchResult::Fail(
			TEXT("queue_corrupt"),
			TEXT("Queued request carried no work callable."));
	}

	return Request.Work();
}

int32 FUnrealOpenMcpBridgeRequestQueue::PendingCount() const
{
	FScopeLock ScopeLock(&Lock);
	int32 Total = 0;
	for (const auto& Pair : AgentQueues)
	{
		Total += Pair.Value.Num();
	}
	return Total;
}

int32 FUnrealOpenMcpBridgeRequestQueue::AgentCount() const
{
	FScopeLock ScopeLock(&Lock);
	int32 Count = 0;
	for (const auto& Pair : AgentQueues)
	{
		if (Pair.Value.Num() > 0)
		{
			++Count;
		}
	}
	return Count;
}

void FUnrealOpenMcpBridgeRequestQueue::CompactAgents()
{
	// (Called under Lock by PickNext.) Remove agents whose queues are empty so
	// AgentOrder stays tight and the cursor doesn't waste cycles on drained
	// agents. Removal preserves the relative order of the survivors.
	for (int32 i = AgentOrder.Num() - 1; i >= 0; --i)
	{
		const FString& AgentId = AgentOrder[i];
		const TArray<FUnrealOpenMcpQueuedRequest>* Queue = AgentQueues.Find(AgentId);
		if (Queue == nullptr || Queue->Num() == 0)
		{
			AgentQueues.Remove(AgentId);
			AgentOrder.RemoveAt(i, 1, false);
			// Clamp the cursor so it stays in range after removal.
			if (AgentOrder.Num() > 0)
			{
				RRCursor %= AgentOrder.Num();
			}
			else
			{
				RRCursor = 0;
			}
		}
	}
}
