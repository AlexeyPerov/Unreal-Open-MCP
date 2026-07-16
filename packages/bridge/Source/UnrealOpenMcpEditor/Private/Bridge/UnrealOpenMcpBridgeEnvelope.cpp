// Tool dispatch envelope builders — see header for the {ok,result,error}
// contract rationale and the intentional P2.1 delta from Unity's gate envelope.
#include "Bridge/UnrealOpenMcpBridgeEnvelope.h"

#include "Bridge/UnrealOpenMcpBridgeJson.h"

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
