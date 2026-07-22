// Auth decision gate. See header for the policy matrix + cross-side contract.
#include "Bridge/UnrealOpenMcpBridgeAuthCheck.h"

#include "Bridge/UnrealOpenMcpBridgeAuthPolicy.h"
#include "Bridge/UnrealOpenMcpBridgeAuthToken.h"

bool FUnrealOpenMcpBridgeAuthCheck::IsAuthorized(
	const FString& Policy, const FString& HeaderValue, const FString& ExpectedToken)
{
	// Only "none" is an explicit opt-out.
	if (Policy == FUnrealOpenMcpBridgeAuthPolicy::None())
	{
		return true;
	}

	// An unrecognized policy (null, corrupt settings, typo) must fail closed —
	// never silently coerce to "required" or "none".
	if (Policy != FUnrealOpenMcpBridgeAuthPolicy::Required())
	{
		return false;
	}

	// "required": an empty expected token (e.g. a lock that lost its token) is
	// never authorized — deny before touching the header.
	if (ExpectedToken.IsEmpty())
	{
		return false;
	}

	const FString Presented = FUnrealOpenMcpBridgeAuthToken::ExtractBearer(HeaderValue);
	if (Presented.IsEmpty())
	{
		return false;
	}

	return FUnrealOpenMcpBridgeAuthToken::EqualsConstantTime(Presented, ExpectedToken);
}
