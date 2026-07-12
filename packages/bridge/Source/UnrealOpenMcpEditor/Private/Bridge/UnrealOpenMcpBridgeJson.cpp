// JSON builders for the bridge HTTP surface.
// See header for the deterministic-field-order rationale (specs/execution/P1/P1.3.md).
#include "Bridge/UnrealOpenMcpBridgeJson.h"

void FUnrealOpenMcpBridgeJson::AppendEscapedContent(FString& Out, const FString& Value)
{
	// Mirror Unity's BridgeJson.EscapeStringContentTo switch (the minimal JSON
	// escape set: the four required quote/backslash/control escapes plus
	// \uXXXX for the rest of the C0 control block). UTF-8 round-tripping is
	// already handled by TCHAR→char at the socket write boundary; this walker
	// only touches ASCII specials.
	for (const TCHAR Ch : Value)
	{
		switch (Ch)
		{
			case TEXT('"'):  Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			default:
				if (static_cast<uint32>(Ch) < 32u)
				{
					Out += FString::Printf(TEXT("\\u%04X"), static_cast<uint32>(Ch));
				}
				else
				{
					Out += Ch;
				}
				break;
		}
	}
}

void FUnrealOpenMcpBridgeJson::AppendJsonString(FString& Out, const FString& Value)
{
	if (Value.IsEmpty())
	{
		Out += TEXT("\"\"");
		return;
	}
	Out += TEXT('"');
	AppendEscapedContent(Out, Value);
	Out += TEXT('"');
}

FString FUnrealOpenMcpBridgeJson::BuildPingJson(const FUnrealOpenMcpPingPayload& Payload)
{
	// Pinned field order — see FUnrealOpenMcpPingPayload. Mirror Unity's
	// BridgeJson.BuildPingJson shape (connected, projectPath, *Version, mode,
	// compiling, isPlaying), with the engine-specific field renamed to
	// unrealVersion and the deterministic port added for parity with P1.4's
	// instance lock. Every field is always present; clients can probe cheaply
	// with a substring search.
	FString Out;
	Out.Reserve(256);
	Out += TEXT('{');

	Out += TEXT("\"connected\":");
	Out += Payload.bConnected ? TEXT("true") : TEXT("false");

	Out += TEXT(",\"status\":");
	AppendJsonString(Out, Payload.Status);

	Out += TEXT(",\"projectPath\":");
	AppendJsonString(Out, Payload.ProjectPath);

	Out += TEXT(",\"unrealVersion\":");
	AppendJsonString(Out, Payload.UnrealVersion);

	Out += TEXT(",\"bridgeVersion\":");
	AppendJsonString(Out, Payload.BridgeVersion);

	Out += TEXT(",\"mode\":");
	AppendJsonString(Out, Payload.Mode);

	Out += TEXT(",\"port\":");
	Out += FString::FromInt(static_cast<int32>(Payload.Port));

	Out += TEXT(",\"compiling\":");
	Out += Payload.bCompiling ? TEXT("true") : TEXT("false");

	Out += TEXT(",\"isPlaying\":");
	Out += Payload.bIsPlaying ? TEXT("true") : TEXT("false");

	Out += TEXT('}');
	return Out;
}

FString FUnrealOpenMcpBridgeJson::BuildErrorJson(const FString& Code, const FString& Message)
{
	FString Out;
	Out.Reserve(128);
	Out += TEXT("{\"error\":{\"code\":");
	AppendJsonString(Out, Code);
	Out += TEXT(",\"message\":");
	AppendJsonString(Out, Message);
	Out += TEXT("}}");
	return Out;
}
