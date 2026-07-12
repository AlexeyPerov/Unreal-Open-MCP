// SHA-256 (FIPS 180-4) for the Unreal Open MCP bridge.
//
// Self-contained, public-domain-style implementation of the SHA-256 hash. Used
// by FUnrealOpenMcpInstancePortResolver (P1.4) to derive a deterministic port
// from the project path and to key the instance-lock file name.
//
// Why a self-contained hash and not FSHA1 / FSHA256:
//   - FSHA1 (Core/Misc/SHA1.h) is SHA-1, NOT SHA-256. Using it here would be a
//     silent algorithm downgrade and break byte-for-byte parity with the TS
//     mirror (mcp-server/src/instance-discovery.ts) and Node
//     crypto.createHash('sha256') — see specs/execution/P1/P1.4.md §Risks.
//   - Unreal ships an OpenSSL-backed FSHA256 only behind the crypto module
//     dependency, which is editor/packaging-version-sensitive; pulling it in
//     just to hash a project path would be a heavy, fragile dependency for a
//     pure-math operation.
//   - SHA-256 is ~120 lines of straight-line C++ with no platform code; a
//     self-contained impl keeps the resolver portable and its output pinned
//     across every platform Unreal builds on. Cross-side consistency with Node
//     is pinned by UnrealOpenMcpPortResolverSpec golden vectors, which match
//     the same golden vectors used by the TS test (instance-discovery.test.ts).
//
// Reference: FIPS 180-4 §6.2. The algorithm is the published unencumbered
// reference implementation; no third-party attribution is required.
#pragma once

#include "CoreMinimal.h"

/**
 * Self-contained SHA-256 (FIPS 180-4).
 *
 * Usage mirrors a streaming hash: create an instance, Update() with bytes,
 * Final() to produce the 32-byte digest. For one-shot hashing of a FString,
 * see HashString / HexDigest.
 */
class UNREALOPENMCPRUNTIME_API FUnrealOpenMcpSha256
{
public:
	FUnrealOpenMcpSha256();

	/** Feed bytes into the hash. May be called multiple times. */
	void Update(const uint8* Data, int32 NumBytes);

	/** Finalize and write the 32-byte digest into OutDigest (length 32). */
	void Final(uint8 OutDigest[32]);

	// ----- one-shot helpers -----

	/**
	 * One-shot SHA-256 of a UTF-8 FString. Writes the 32-byte digest into
	 * OutDigest. The most common entry point (hashing the project path).
	 */
	static void HashString(const FString& Value, uint8 OutDigest[32]);

	/**
	 * One-shot SHA-256 of a UTF-8 FString rendered as 64 lowercase hex chars —
	 * the form used for the lock file name and the projectHash field. Mirrors
	 * Node crypto.createHash('sha256').update(s).digest('hex') byte-for-byte.
	 */
	static FString HexDigest(const FString& Value);

private:
	/** Process a full 64-byte block. */
	void ProcessBlock(const uint8* Block);

	uint32 State[8];
	uint64 BitLength;
	uint8 Buffer[64];
	int32 BufferLength;
};
