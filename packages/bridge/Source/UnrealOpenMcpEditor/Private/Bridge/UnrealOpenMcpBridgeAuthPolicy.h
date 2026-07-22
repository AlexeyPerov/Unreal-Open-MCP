// Auth policy constants + validity.
//
// Ports Unity Open MCP's BridgeAuthPolicy
// (packages/bridge/Editor/Bridge/BridgeAuthPolicy.cs) — the string constants
// for the authMode settings value and the IsValid predicate. Fidelity: copy.
//
// The authMode lives in <project>/.unreal-open-mcp/settings.json and is read
// by FUnrealOpenMcpBridgeProjectSettings. It defaults to "none" (open by
// default; the token is still minted so a project can flip to "required" with
// no restart). Only the exact string "required" turns enforcement on; any
// other value (null/corrupt/typo) is treated as unknown and the auth check
// fails closed.
#pragma once

#include "CoreMinimal.h"

/**
 * Static auth-policy constants + validation. Mirrors Unity's BridgeAuthPolicy.
 */
struct UNREALOPENMCPEDITOR_API FUnrealOpenMcpBridgeAuthPolicy
{
	/** Open mode — no Bearer header required (the default). */
	static constexpr const TCHAR* None() { return TEXT("none"); }

	/** Required mode — every request must carry a valid Bearer token. */
	static constexpr const TCHAR* Required() { return TEXT("required"); }

	/** The default mode when the settings file omits authMode. */
	static constexpr const TCHAR* Default() { return None(); }

	/**
	 * True only for the exact "none" or "required" strings. Everything else
	 * (including empty) is invalid and the auth check treats it as fail-closed.
	 */
	static bool IsValid(const FString& Mode)
	{
		return Mode == None() || Mode == Required();
	}
};
