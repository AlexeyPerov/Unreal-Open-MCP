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
	 * Mint a fresh 32-byte token from the OS cryptographic RNG, hex-encoded to 64
	 * lowercase chars. Never returns empty.
	 *
	 * Entropy source (see the .cpp for detail):
	 *   - Windows  → BCryptGenRandom (BCRYPT_USE_SYSTEM_PREFERRED_RNG)
	 *   - Mac/Linux → /dev/urandom
	 *   - Fallback (OS source unavailable) → SHA-256 hash extraction over a
	 *     multi-source entropy mix, with a Warning logged so the degradation is
	 *     visible rather than silent.
	 *
	 * NOT FRandomStream. An earlier implementation seeded an FRandomStream from a
	 * time/cycle/PID/stack mix and drew all 32 bytes from it — but FRandomStream
	 * is a 32-bit-state LCG, so the "256-bit" token was a deterministic function
	 * of at most 2^32 states, dominated by the wall clock. This token is the sole
	 * credential guarding a remote-bindable tool surface that can run console
	 * commands and invoke arbitrary UFunctions, so it must come from a CSPRNG.
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
