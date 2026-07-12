// UnrealOpenMcpBridgeSession — version + session surface for the editor bridge.
//
// Mirrors Unity Open MCP's BridgeSession.BridgeVersion (a single static string
// the HTTP bridge advertises on /ping and the MCP server reads for parity
// checks). The version string is the single source of truth for the C++ side
// and is kept in lockstep with version.json by scripts/sync-version.mjs.
//
// DO NOT hand-edit BRIDGE_VERSION — run `node scripts/sync-version.mjs` from
// the repo root after bumping version.json. The sync gate in CI
// (.github/workflows/version-sync.yml) fails any PR where this constant drifts
// from the shared trio version.
#pragma once

#include "CoreMinimal.h"

struct UNREALOPENMCPEDITOR_API FUnrealOpenMcpBridgeSession
{
	// The plugin/bridge version advertised to MCP clients. Synced from
	// <repo>/version.json by scripts/sync-version.mjs.
	static const TCHAR* GetBridgeVersion() { return BRIDGE_VERSION; }

private:
	// Auto-managed by scripts/sync-version.mjs — do not edit by hand.
	static constexpr const TCHAR* BRIDGE_VERSION = TEXT("0.0.1");
};
