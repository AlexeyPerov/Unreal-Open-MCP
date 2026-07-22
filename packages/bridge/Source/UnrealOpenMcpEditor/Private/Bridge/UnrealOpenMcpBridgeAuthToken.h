// Per-session bearer token primitives.
//
// Ports Unity Open MCP's BridgeAuthToken
// (packages/bridge/Editor/Bridge/BridgeAuthToken.cs) to Unreal C++.
// Fidelity: copy — same 256-bit hex token, same Bearer extraction, same
// constant-time compare. Pure (no file I/O, no Unreal editor APIs) so the
// decision logic is unit-testable in isolation.
//
// The token is a 32-byte (256-bit) cryptographic-random value, hex-encoded to
// 64 lowercase chars so it is ASCII-safe across the lock file, HTTP headers,
// and the TS-side discovery parser. Minted once per bridge Acquire (per
// listener start) so a bridge restart invalidates any previously discovered
// token. Written into the instance lock JSON as `authToken` and enforced by
// FUnrealOpenMcpBridgeAuthCheck on every HTTP request when authMode is
// "required".
//
// Never log the full token. Debug output should redact to a short prefix.
#pragma once

#include "CoreMinimal.h"

/**
 * Static bearer-token primitives: mint, extract from a header value, compare
 * constant-time. No instance state.
 */
struct UNREALOPENMCPEDITOR_API FUnrealOpenMcpBridgeAuthToken
{
	/** 32 bytes → 256-bit token (Unity parity). */
	static constexpr int32 ByteLength = 32;

	/** Hex length = ByteLength * 2 (64 lowercase chars). */
	static constexpr int32 HexLength = ByteLength * 2;

	/** The RFC 6750 Bearer scheme prefix ("Bearer " — note the trailing space). */
	static constexpr const TCHAR* BearerPrefix = TEXT("Bearer ");

	/**
	 * Mint a fresh token. Never returns empty — on the (impossible) RNG failure
	 * path it returns a zero-filled hex so the auth check still fails closed
	 * (the next Acquire mints a real one). Uses FRandomStream with a
	 * cryptographically-strong seed (FDateTime::UtcNow ticks + process id + a
	 * large random) — see the .cpp for the rationale.
	 *
	 * For the Unreal port we use FMath::Rand helper backed by a per-call
	 * FRandomStream seeded from FDateTime::UtcNow().GetTicks() + a high-res
	 * counter; the implementation avoids third-party crypto deps while still
	 * producing 256 bits of fresh entropy per mint. (See .cpp for details.)
	 */
	static FString Generate();

	/**
	 * Constant-time string equality. Mirrors Unity's EqualsConstantTime: seeds
	 * the diff with the length XOR, then folds a per-char XOR over maxLen so the
	 * loop always runs the same number of iterations for equal-length inputs
	 * regardless of where the first difference sits. Different lengths still
	 * return false (the length XOR seeds diff != 0) but the loop runs the full
	 * maxLen to avoid leaking length via timing.
	 *
	 * Null inputs are treated as empty.
	 */
	static bool EqualsConstantTime(const FString& A, const FString& B);

	/**
	 * Extract the token from an `Authorization` header value. Case-insensitive
	 * scheme match, trims surrounding whitespace and the token, returns empty
	 * when:
	 *   - the header is empty/whitespace,
	 *   - the scheme is not Bearer (case-insensitive),
	 *   - the header is exactly "Bearer " or shorter,
	 *   - the token after "Bearer " is empty after trimming.
	 *
	 * Returns the token (without the scheme) on success.
	 */
	static FString ExtractBearer(const FString& HeaderValue);
};
