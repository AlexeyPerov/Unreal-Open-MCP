// Tool dispatch envelope builders — see header for the {ok,result,error}
// contract rationale and the P3.5 widening.
#include "Bridge/UnrealOpenMcpBridgeEnvelope.h"

#include "Bridge/UnrealOpenMcpBridgeJson.h"

namespace UnrealOpenMcpEnvelopeInternal
{
	// Wire token for a gate mode. Emitted in paths_hint_required so the agent
	// sees which gate mode was applied when the hint was missing.
	const TCHAR* GateModeToken(EUnrealOpenMcpGateMode Mode)
	{
		switch (Mode)
		{
			case EUnrealOpenMcpGateMode::Enforce: return TEXT("enforce");
			case EUnrealOpenMcpGateMode::Warn: return TEXT("warn");
			case EUnrealOpenMcpGateMode::Off: return TEXT("off");
		}
		return TEXT("enforce");
	}

	// Emit the gate summary object. Pinned field order so a parser can branch
	// on `gate.outcome` without scanning. Optional fields (checkpointId, delta,
	// timing, categoriesRun, agentNextSteps) are emitted only when meaningful
	// — the gate-ran / outcome / gateFailed trio is always present so a parser
	// has the load-bearing signal in every dispatch.
	void AppendGateSummary(FString& Out, const FUnrealOpenMcpGateDispatchResult& GateResult)
	{
		Out += TEXT("\"gate\":{");
		Out += TEXT("\"ran\":");
		Out += GateResult.bGateRan ? TEXT("true") : TEXT("false");
		Out += TEXT(",\"outcome\":\"");
		Out += FUnrealOpenMcpGatePolicy::OutcomeToken(GateResult.Outcome);
		Out += TEXT("\",\"gateFailed\":");
		Out += GateResult.bGateFailed ? TEXT("true") : TEXT("false");

		if (!GateResult.CheckpointId.IsEmpty())
		{
			Out += TEXT(",\"checkpointId\":");
			FUnrealOpenMcpBridgeJson::AppendJsonString(Out, GateResult.CheckpointId);
		}

		if (GateResult.Delta.IsSet())
		{
			const FUnrealOpenMcpGateDelta& D = *GateResult.Delta;
			Out += TEXT(",\"delta\":{");
			Out += FString::Printf(TEXT("\"newErrors\":%d,\"newWarnings\":%d,\"resolvedErrors\":%d,\"resolvedWarnings\":%d"),
				D.NewErrors, D.NewWarnings, D.ResolvedErrors, D.ResolvedWarnings);
			Out += TEXT(",\"newIssueKeys\":[");
			for (int32 i = 0; i < D.NewIssueKeys.Num(); ++i)
			{
				if (i > 0) Out += TEXT(',');
				FUnrealOpenMcpBridgeJson::AppendJsonString(Out, D.NewIssueKeys[i]);
			}
			Out += TEXT("],\"resolvedIssueKeys\":[");
			for (int32 i = 0; i < D.ResolvedIssueKeys.Num(); ++i)
			{
				if (i > 0) Out += TEXT(',');
				FUnrealOpenMcpBridgeJson::AppendJsonString(Out, D.ResolvedIssueKeys[i]);
			}
			Out += TEXT("]}");
		}

		if (GateResult.CategoriesRun.Num() > 0)
		{
			Out += TEXT(",\"categoriesRun\":[");
			for (int32 i = 0; i < GateResult.CategoriesRun.Num(); ++i)
			{
				if (i > 0) Out += TEXT(',');
				FUnrealOpenMcpBridgeJson::AppendJsonString(Out, GateResult.CategoriesRun[i]);
			}
			Out += TEXT("]");
		}

		if (GateResult.bGateRan)
		{
			Out += FString::Printf(
				TEXT(",\"checkpointMs\":%lld,\"validateMs\":%lld,\"totalMs\":%lld"),
				GateResult.CheckpointDurationMs,
				GateResult.ValidationDurationMs,
				GateResult.TotalGateDurationMs);
		}

		if (GateResult.AgentNextSteps.Num() > 0)
		{
			Out += TEXT(",\"agentNextSteps\":[");
			for (int32 i = 0; i < GateResult.AgentNextSteps.Num(); ++i)
			{
				if (i > 0) Out += TEXT(',');
				FUnrealOpenMcpBridgeJson::AppendJsonString(Out, GateResult.AgentNextSteps[i]);
			}
			Out += TEXT("]");
		}

		Out += TEXT('}');
	}
} // namespace UnrealOpenMcpEnvelopeInternal

FString FUnrealOpenMcpBridgeEnvelope::BuildSuccess(const FString& ResultJson)
{
	// Splice the pre-serialized result JSON verbatim. An empty/whitespace-only
	// result is emitted as `null` so the envelope always carries a value (a
	// tool that returns nothing is a `null` result, not a missing field).
	FString Out;
	Out.Reserve(32 + ResultJson.Len());
	Out += TEXT("{\"ok\":true,\"result\":");
	if (ResultJson.TrimStartAndEnd().IsEmpty())
	{
		Out += TEXT("null");
	}
	else
	{
		Out += ResultJson;
	}
	Out += TEXT('}');
	return Out;
}

FString FUnrealOpenMcpBridgeEnvelope::BuildSuccessWithGate(
	const FString& ResultJson,
	const FUnrealOpenMcpGateDispatchResult& GateResult)
{
	namespace Internal = UnrealOpenMcpEnvelopeInternal;
	FString Out;
	Out.Reserve(64 + ResultJson.Len());
	Out += TEXT("{\"ok\":true,\"result\":");
	if (ResultJson.TrimStartAndEnd().IsEmpty())
	{
		Out += TEXT("null");
	}
	else
	{
		Out += ResultJson;
	}
	Out += TEXT(',');
	Internal::AppendGateSummary(Out, GateResult);
	Out += TEXT('}');
	return Out;
}

FString FUnrealOpenMcpBridgeEnvelope::BuildSuccessWithGateAndRollback(
	const FString& ResultJson,
	const FUnrealOpenMcpGateDispatchResult& GateResult,
	const FApplyFixRollbackFields& Rollback)
{
	namespace Internal = UnrealOpenMcpEnvelopeInternal;
	FString Out;
	Out.Reserve(96 + ResultJson.Len());
	Out += TEXT("{\"ok\":true,\"result\":");
	if (ResultJson.TrimStartAndEnd().IsEmpty())
	{
		Out += TEXT("null");
	}
	else
	{
		Out += ResultJson;
	}
	Out += TEXT(',');
	Internal::AppendGateSummary(Out, GateResult);

	// Rollback block — emitted only when one of the rollback signals is set.
	// A clean apply_fix pass (gate passed, no rollback) does NOT emit the
	// block so the wire shape is identical to BuildSuccessWithGate.
	if (Rollback.bRolledBack || Rollback.bRollbackDisabled)
	{
		Out += TEXT(",\"rollback\":{");
		Out += TEXT("\"rolledBack\":");
		Out += Rollback.bRolledBack ? TEXT("true") : TEXT("false");
		if (Rollback.bRollbackDisabled)
		{
			Out += TEXT(",\"rollbackDisabled\":true");
		}
		if (!Rollback.RollbackReason.IsEmpty())
		{
			Out += TEXT(",\"reason\":");
			FUnrealOpenMcpBridgeJson::AppendJsonString(Out, Rollback.RollbackReason);
		}
		if (Rollback.RestoredPaths.Num() > 0)
		{
			Out += TEXT(",\"restoredPaths\":[");
			for (int32 i = 0; i < Rollback.RestoredPaths.Num(); ++i)
			{
				if (i > 0) Out += TEXT(',');
				FUnrealOpenMcpBridgeJson::AppendJsonString(Out, Rollback.RestoredPaths[i]);
			}
			Out += TEXT(']');
		}
		Out += TEXT('}');
	}
	Out += TEXT('}');
	return Out;
}

FString FUnrealOpenMcpBridgeEnvelope::BuildError(const FString& Code, const FString& Message)
{
	// {ok:false, error:{code, message}} — the error object reuses the same
	// code+message pair the existing BuildErrorJson emits for HTTP-level
	// errors, so the MCP server unwraps both shapes with one parser.
	FString Out;
	Out.Reserve(64 + Code.Len() + Message.Len());
	Out += TEXT("{\"ok\":false,\"error\":{\"code\":");
	FUnrealOpenMcpBridgeJson::AppendJsonString(Out, Code);
	Out += TEXT(",\"message\":");
	FUnrealOpenMcpBridgeJson::AppendJsonString(Out, Message);
	Out += TEXT("}}");
	return Out;
}

FString FUnrealOpenMcpBridgeEnvelope::BuildErrorWithGate(
	const FString& Code,
	const FString& Message,
	const FUnrealOpenMcpGateDispatchResult& GateResult)
{
	namespace Internal = UnrealOpenMcpEnvelopeInternal;
	FString Out;
	Out.Reserve(96 + Code.Len() + Message.Len());
	Out += TEXT("{\"ok\":false,\"error\":{\"code\":");
	FUnrealOpenMcpBridgeJson::AppendJsonString(Out, Code);
	Out += TEXT(",\"message\":");
	FUnrealOpenMcpBridgeJson::AppendJsonString(Out, Message);
	Out += TEXT("},");
	Internal::AppendGateSummary(Out, GateResult);
	Out += TEXT('}');
	return Out;
}

FString FUnrealOpenMcpBridgeEnvelope::BuildToolNotFound(const FString& ToolName)
{
	// Bare {error:{code,message}} — NOT wrapped in {ok,...} because
	// tool_not_found is an HTTP-level routing failure (404), parallel to the
	// existing BuildErrorJson used for not_found / method_not_allowed. The MCP
	// side reads .error.code uniformly from both 4xx and 200+{ok:false} bodies.
	return FUnrealOpenMcpBridgeJson::BuildErrorJson(
		TEXT("tool_not_found"),
		FString::Printf(TEXT("Unknown tool: %s"), *ToolName));
}

FString FUnrealOpenMcpBridgeEnvelope::BuildPathsHintRequired(
	const FString& ToolName, EUnrealOpenMcpGateMode EffectiveGate)
{
	namespace Internal = UnrealOpenMcpEnvelopeInternal;
	FString Out;
	Out.Reserve(128 + ToolName.Len());
	Out += TEXT("{\"ok\":false,\"error\":{\"code\":\"paths_hint_required\",\"message\":");
	FUnrealOpenMcpBridgeJson::AppendJsonString(
		Out,
		FString::Printf(
			TEXT("Mutating tool '%s' requires a non-empty paths_hint (content paths the mutation is scoped to). There is no whole-project fallback — re-issue with paths_hint, or set gate:\"off\" to skip the gate."),
			*ToolName));
	Out += TEXT("},\"gate\":{\"ran\":false,\"outcome\":\"failed\",\"gateFailed\":true,\"effectiveMode\":\"");
	Out += Internal::GateModeToken(EffectiveGate);
	Out += TEXT("\"}}");
	return Out;
}
