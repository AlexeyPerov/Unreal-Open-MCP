// Bind-address policy: loopback by default, remote only with auth.
//
// Ports Unity Open MCP's BridgeBindAddress
// (packages/bridge/Editor/Bridge/BridgeBindAddress.cs). Fidelity: copy — the
// Decide() matrix is pure (no I/O, no Unreal APIs) so it is unit-testable.
//
// The bridge binds 127.0.0.1 by default. A project can opt into 0.0.0.0
// (remote) via bindAddress in settings.json, BUT only when authMode is
// "required" — a remote listener without token auth is a footgun the bridge
// refuses to start. This couples the bind decision to the auth policy so a
// misconfigured project fails fast at Start() instead of exposing an open
// port.
//
// Decision matrix (pinned by UnrealOpenMcpBridgeAuthSpec):
//   - bindAddress "127.0.0.1"             → allow loopback (any authMode)
//   - bindAddress "0.0.0.0" + authMode
//     "required"                          → allow remote
//   - bindAddress "0.0.0.0" + authMode
//     not "required"                      → refuse (with the threat-model msg)
//   - invalid bindAddress                 → coerce to loopback → allow
#pragma once

#include "CoreMinimal.h"

/**
 * Bind-address decision over (bindAddress, authMode). Static; pure.
 * Mirrors Unity's BridgeBindAddress.
 */
struct UNREALOPENMCPEDITOR_API FUnrealOpenMcpBridgeBindAddress
{
	/** Loopback bind literal. */
	static constexpr const TCHAR* Loopback() { return TEXT("127.0.0.1"); }

	/** Remote (all-interfaces) bind literal. */
	static constexpr const TCHAR* Remote() { return TEXT("0.0.0.0"); }

	/** Default bind when settings omit bindAddress or hold an invalid value. */
	static constexpr const TCHAR* Default() { return Loopback(); }

	/** True only for the exact loopback or remote literal. */
	static bool IsValid(const FString& Address)
	{
		return Address == Loopback() || Address == Remote();
	}

	/** True only for the remote literal. */
	static bool IsRemote(const FString& Address) { return Address == Remote(); }

	/**
	 * Outcome of the bind decision. ResolvedAddress is always the address the
	 * server SHOULD bind (loopback on coerce, the requested address on allow).
	 * When Allowed is false, RefusalReason carries the operator-facing message.
	 */
	struct FBindDecision
	{
		bool bAllowed = false;
		FString ResolvedAddress;
		FString RefusalReason;
	};

	/**
	 * Pure decision over (bindAddress, authMode). authMode is the canonical
	 * policy string (NOT pre-validated — an unknown mode here still refuses a
	 * remote bind because the check is `!= Required`). Invalid bind addresses
	 * coerce to loopback (the safe default), never to remote.
	 */
	static FBindDecision Decide(const FString& BindAddress, const FString& AuthMode);
};
