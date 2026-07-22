// Bind-address policy. See header for the decision matrix.
#include "Bridge/UnrealOpenMcpBridgeBindAddress.h"

#include "Bridge/UnrealOpenMcpBridgeAuthPolicy.h"

FUnrealOpenMcpBridgeBindAddress::FBindDecision FUnrealOpenMcpBridgeBindAddress::Decide(
	const FString& BindAddress, const FString& AuthMode)
{
	// Invalid bind addresses coerce to the safe loopback default — never to
	// remote. A typo in settings.json must not silently open the bridge.
	const FString Resolved = IsValid(BindAddress) ? BindAddress : FString(Default());

	FBindDecision Out;
	Out.ResolvedAddress = Resolved;

	// Loopback is always allowed regardless of auth mode — it is only
	// reachable from the same machine.
	if (!IsRemote(Resolved))
	{
		Out.bAllowed = true;
		return Out;
	}

	// Remote (0.0.0.0) requires authMode "required" exactly. A null/corrupt
	// auth mode refuses (fail-closed) — the operator must explicitly opt into
	// both remote bind AND token auth before the bridge opens to the network.
	if (AuthMode != FUnrealOpenMcpBridgeAuthPolicy::Required())
	{
		Out.bAllowed = false;
		Out.RefusalReason = FString::Printf(
			TEXT("Remote bind (0.0.0.0) requires authMode \"required\". The bridge refuses to ")
			TEXT("start on a non-loopback interface without token auth — set authMode to ")
			TEXT("\"required\" in .unreal-open-mcp/settings.json before enabling remote bind. ")
			TEXT("See docs/api/bridge-http.md §Remote bind for the threat model."));
		return Out;
	}

	Out.bAllowed = true;
	return Out;
}
