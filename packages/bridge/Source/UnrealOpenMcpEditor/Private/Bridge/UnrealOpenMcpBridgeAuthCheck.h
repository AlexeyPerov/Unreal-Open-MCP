// Auth decision gate (pure).
//
// Ports Unity Open MCP's BridgeAuthCheck
// (packages/bridge/Editor/Bridge/BridgeAuthCheck.cs). Fidelity: copy — the
// IsAuthorized matrix is the load-bearing security primitive and must match
// Unity byte-for-byte in behavior. Pure (no I/O, no Unreal APIs) so it is
// unit-testable in isolation.
//
// Policy matrix (see UnrealOpenMcpBridgeAuthSpec for the pinned cases):
//   - policy "none"                       → allow (ignore header + token)
//   - policy anything other than
//     "none"/"required" (null/typo/corrupt)→ deny (fail closed)
//   - policy "required" + empty expected  → deny
//   - policy "required" + no/bad Bearer   → deny
//   - policy "required" + valid Bearer    → constant-time compare → allow/deny
//
// The check never throws and never logs the token. Callers (the HTTP server)
// own writing the 401 response when IsAuthorized returns false.
#pragma once

#include "CoreMinimal.h"

/**
 * Static auth decision. Mirrors Unity's BridgeAuthCheck.
 */
struct UNREALOPENMCPEDITOR_API FUnrealOpenMcpBridgeAuthCheck
{
	/**
	 * Return true when the request should be allowed to proceed.
	 *
	 * @param Policy         the authMode string ("none" / "required" / other)
	 * @param HeaderValue    the raw `Authorization` header value (may be empty)
	 * @param ExpectedToken  the per-session token from the instance lock
	 */
	static bool IsAuthorized(const FString& Policy, const FString& HeaderValue, const FString& ExpectedToken);
};
