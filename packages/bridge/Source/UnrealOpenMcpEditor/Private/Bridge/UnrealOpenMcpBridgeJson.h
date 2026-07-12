// JSON builders for the bridge HTTP surface.
//
// Adapts Unity Open MCP's BridgeJson hand-rolled builders
// (packages/bridge/Editor/Bridge/BridgeJson.cs) to Unreal. The bridge
// deliberately has no JSON-object serializer dependency for the health surface:
// every body is assembled with explicit FString accumulation so the field order
// is deterministic and the P1.3 ping spec can pin it byte-for-byte. (An
// FJsonObject-backed writer would iterate a map and leave field order
// implementation-defined — fine for tool payloads, not for a pinned health
// contract that the MCP server probes programmatically.)
//
// P1.3 scope: /ping health body + HTTP error body only. Gate / tool envelopes
// land in later phases alongside BridgeHttpServer's POST /tools/{name} route.
#pragma once

#include "CoreMinimal.h"

/**
 * Immutable field set for the /ping health body. Built on the game thread (see
 * FUnrealOpenMcpBridgeHttpServer::HandlePing) and serialized in a pinned order
 * by BuildPingJson. Every field is always emitted so clients can parse
 * defensively without conditional presence checks.
 */
struct UNREALOPENMCPEDITOR_API FUnrealOpenMcpPingPayload
{
	/** True only when the game thread responded to the ping dispatch — the
	 *  load-bearing readiness signal. 503 fallbacks carry false. */
	bool bConnected = false;
	/** Coarse readiness label: "ready" (HTTP 200) or "not_ready" (503). Later
	 *  phases may add "compiling". */
	FString Status;
	/** Absolute project directory, empty when unknown (503 fallback). */
	FString ProjectPath;
	/** Engine version string from FEngineVersion::Current(), empty when unknown. */
	FString UnrealVersion;
	/** Bridge/plugin version, mirrored from FUnrealOpenMcpBridgeSession. */
	FString BridgeVersion;
	/** Transport mode label — always "live" for the HTTP bridge. */
	FString Mode;
	/** Bound loopback port. */
	uint16 Port = 0;
	/** Stub false in P1.3 — real compile state lands in P5.7 (bridge_status). */
	bool bCompiling = false;
	/** Stub false in P1.3 — real PIE state lands in P5.7 (bridge_status). */
	bool bIsPlaying = false;
};

/**
 * Minimal JSON builder for the bridge HTTP surface. Static-only; no instance
 * state. Mirrors Unity's BridgeJson contract (EscapeString + AppendJson*
 * primitives + BuildPingJson) adapted to FString.
 */
struct UNREALOPENMCPEDITOR_API FUnrealOpenMcpBridgeJson
{
	/**
	 * Build the deterministic /ping health JSON. Field order is PINNED by the
	 * P1.3 spec and must not change without bumping the contract — the MCP
	 * server and tests depend on stable key order for cheap substring probes.
	 *
	 * Field set: connected, status, projectPath, unrealVersion, bridgeVersion,
	 * mode, port, compiling, isPlaying.
	 */
	static FString BuildPingJson(const FUnrealOpenMcpPingPayload& Payload);

	/**
	 * Build a flat {"error":{"code":...,"message":...}} body for HTTP error
	 * responses (404 / 405 / 400). Mirrors Unity's BridgeHttpResponse.SendJsonError
	 * shape so the MCP server can map bridge HTTP errors uniformly.
	 */
	static FString BuildErrorJson(const FString& Code, const FString& Message);

	/**
	 * Append a JSON-escaped string value (with surrounding quotes) to Out.
	 * Public so later phases can compose envelopes without re-implementing the
	 * escape switch. Empty values are emitted as `""`.
	 */
	static void AppendJsonString(FString& Out, const FString& Value);

private:
	/** Escape the contents of Value (no surrounding quotes) into Out. */
	static void AppendEscapedContent(FString& Out, const FString& Value);
};
