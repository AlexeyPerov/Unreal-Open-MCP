// Per-project deterministic port + instance-lock path resolver.
//
// Ports Unity Open MCP's InstancePortResolver
// (packages/bridge/Editor/Bridge/InstancePortResolver.cs) to Unreal C++.
// Fidelity: copy — the formula and normalization MUST stay byte-for-byte
// identical to both the Unity bridge and the TS MCP server mirror
// (mcp-server/src/instance-discovery.ts) so all three sides agree on the same
// port for the same project path with zero shared config. Cross-side
// consistency is pinned by golden vectors in UnrealOpenMcpPortResolverSpec
// (these same paths are pinned in InstancePortResolverTests.cs and the TS
// instance-discovery.test.ts).
//
// Why Runtime placement: the resolver is pure math + path string handling — no
// editor APIs, no file I/O in this type (the lock file writer is a separate
// type in the Editor module). Keeping it in Runtime lets a future packaged
// commandlet derive its port without dragging in editor code, preserving the
// Editor→Runtime boundary invariant.
//
// P1.4 scope: deterministic port + lock path + env/CLI override precedence.
//   - authToken minting is deferred to P5.6 (the lock omits the field; absence
//     is pinned in the spec).
//   - The TS discovery parity lands in P1.6 — the formula here is the source of
//     truth both sides pin to.
#pragma once

#include "CoreMinimal.h"

/**
 * Per-project deterministic port + instance-lock path resolver.
 *
 * All methods are static and pure — no instance state, safe to call from any
 * thread. Path normalization + the SHA-256 hash are deliberately inline-able
 * constants so a future TS port can mirror them exactly.
 */
class UNREALOPENMCPRUNTIME_API FUnrealOpenMcpInstancePortResolver
{
public:
	/** Port range is [PortRangeStart, PortRangeStart + PortRangeSize - 1].
	 *  Mirrors InstancePortResolver.PortRangeStart in Unity. */
	static constexpr int32 PortRangeStart = 20000;
	static constexpr int32 PortRangeSize = 10000;

	/** Settings dir name under the user home: ~/.unreal-open-mcp.
	 *  Mirrors Unity's BridgeConstants.SettingsDirName, renamed for Unreal. */
	static constexpr const TCHAR* SettingsDirName = TEXT(".unreal-open-mcp");

	/** Subdirectory under the settings dir that holds per-instance lock files. */
	static constexpr const TCHAR* InstancesSubdir = TEXT("instances");

	// ----- normalization + hashing -----

	/**
	 * Normalize a project path before hashing:
	 *   - backslashes → forward slashes
	 *   - trim trailing slashes (a lone "/" is preserved)
	 *   - case is preserved (NOT lowercased — macOS/Linux paths are
	 *     case-sensitive; lowercasing would collide distinct projects)
	 *
	 * Mirrors instance-discovery.ts normalizePath byte-for-byte.
	 * Empty input returns "".
	 */
	static FString NormalizePath(const FString& ProjectPath);

	/**
	 * SHA-256 of the normalized path as 64 lowercase hex chars. Used both as
	 * the lock file name and as the projectHash field written into the lock
	 * JSON so the MCP server can verify it matched the project it expected.
	 */
	static FString ProjectHash(const FString& ProjectPath);

	// ----- port formula -----

	/**
	 * Deterministic port for a project path:
	 *   PortRangeStart + (first-8-bytes-of-SHA256-as-big-endian-UInt64 % PortRangeSize)
	 *
	 * The 8-byte prefix keeps the modulo inside the UInt64 range so C++
	 * (uint64), C# (UInt64), and TypeScript (BigInt) all agree exactly; a full
	 * 256-bit modulo would diverge across language BigInts. Mirrors
	 * InstancePortResolver.ComputePort and instance-discovery.ts computePort.
	 */
	static int32 ComputePort(const FString& ProjectPath);

	/** A port is valid for binding when it is in [1, 65535]. */
	static bool IsValidPort(int32 Port) { return Port >= 1 && Port <= 65535; }

	/**
	 * Resolve the bridge port with override precedence:
	 *   1. EnvPort  (UNREAL_OPEN_MCP_BRIDGE_PORT env var, caller-parsed)
	 *   2. CliPort  (-UNREAL_OPEN_MCP_BRIDGE_PORT=<n> CLI arg, caller-parsed)
	 *   3. deterministic hash of the project path
	 *
	 * Pass TOptional<int32>() when the caller found no override; IsValidPort is
	 * checked here so the resolver only trusts values the caller already read.
	 * Invalid overrides fall through to the next source rather than being
	 * trusted.
	 */
	static int32 ResolvePort(
		const FString& ProjectPath,
		const TOptional<int32>& EnvPort = TOptional<int32>(),
		const TOptional<int32>& CliPort = TOptional<int32>());

	// ----- paths -----

	/**
	 * Absolute path to the instances dir: <UserHome>/.unreal-open-mcp/instances.
	 * Production callers should pass nothing (the real home is resolved from
	 * the platform). The optional Override is a test-only hook so specs don't
	 * write into the real ~/.unreal-open-mcp.
	 */
	static FString GetInstancesDir(const FString& Override = FString());

	/**
	 * Absolute path to the lock file for a project:
	 *   <instances>/<projectHash>.json
	 * Throws nothing — returns empty when ProjectPath is empty.
	 */
	static FString GetLockPath(const FString& ProjectPath, const FString& InstancesDirOverride = FString());
};
