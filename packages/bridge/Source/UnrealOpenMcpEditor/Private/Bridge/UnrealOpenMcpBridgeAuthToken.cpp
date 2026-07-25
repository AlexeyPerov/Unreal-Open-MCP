// Per-session bearer token primitives. See header for the cross-side contract.
#include "Bridge/UnrealOpenMcpBridgeAuthToken.h"

#include "Crypto/UnrealOpenMcpSha256.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "UnrealOpenMcpLog.h"

#if PLATFORM_WINDOWS
// BCryptGenRandom is the documented Windows CSPRNG. NOTE: rand_s is NOT usable
// here — it only declares itself when _CRT_RAND_S is defined BEFORE <stdlib.h>,
// and CoreMinimal.h (via HAL/PlatformCrt.h) plus the force-included shared PCH
// have both already pulled <stdlib.h> in before line 1 of this file, so a
// mid-file #define can never take effect.
// bcrypt.lib is linked via UnrealOpenMcpEditor.Build.cs (PublicSystemLibraries).
#	include "Windows/AllowWindowsPlatformTypes.h"
#	include <bcrypt.h>
#	include "Windows/HideWindowsPlatformTypes.h"
#else
#	include <stdio.h>
#endif

namespace
{
// Lowercase hex nibble for a 0..15 value. Matches C#'s "x2" format.
TCHAR HexNibble(uint8 Nibble)
{
	return Nibble < 10 ? static_cast<TCHAR>('0' + Nibble) : static_cast<TCHAR>('a' + (Nibble - 10));
}

/**
 * Fill OutBytes from the OS cryptographic RNG. Returns false when the platform
 * source is unavailable, in which case the caller must use the hash-extraction
 * fallback (and log about it).
 *
 * Windows → BCryptGenRandom. Mac/Linux → /dev/urandom. Those are the only
 * platforms in the plugin's PlatformAllowList.
 */
bool TryFillFromOsCsprng(uint8* OutBytes, int32 NumBytes)
{
#if PLATFORM_WINDOWS
	const NTSTATUS Status = BCryptGenRandom(
		/*hAlgorithm=*/nullptr,
		reinterpret_cast<PUCHAR>(OutBytes),
		static_cast<ULONG>(NumBytes),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	return Status >= 0; // BCRYPT_SUCCESS(Status)
#else
	FILE* Urandom = fopen("/dev/urandom", "rb");
	if (Urandom == nullptr)
	{
		return false;
	}
	const size_t Read = fread(OutBytes, 1, static_cast<size_t>(NumBytes), Urandom);
	fclose(Urandom);
	return Read == static_cast<size_t>(NumBytes);
#endif
}

/**
 * Fallback entropy: SHA-256 hash-extraction over a multi-source mix, one block
 * per 32 output bytes. Strictly worse than the OS CSPRNG, but it preserves the
 * full width of the inputs instead of funnelling them through a 32-bit PRNG
 * state. Only reached if the platform RNG is missing.
 */
void FillFromHashExtraction(uint8* OutBytes, int32 NumBytes)
{
	int32 Written = 0;
	uint32 Counter = 0;
	while (Written < NumBytes)
	{
		int32 StackAnchor = 0;
		const FString Seed = FString::Printf(
			TEXT("%llu|%llu|%u|%llu|%s|%u"),
			static_cast<uint64>(FDateTime::UtcNow().GetTicks()),
			static_cast<uint64>(FPlatformTime::Cycles64()),
			FPlatformProcess::GetCurrentProcessId(),
			static_cast<uint64>(reinterpret_cast<uintptr_t>(&StackAnchor)),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits),
			Counter++);

		uint8 Digest[32];
		FUnrealOpenMcpSha256::HashString(Seed, Digest);

		const int32 Chunk = FMath::Min<int32>(32, NumBytes - Written);
		FMemory::Memcpy(OutBytes + Written, Digest, Chunk);
		Written += Chunk;
	}
}
} // end anonymous namespace

FString FUnrealOpenMcpBridgeAuthToken::Generate()
{
	// Mint 32 bytes of fresh entropy and hex-encode to 64 lowercase chars.
	//
	// This MUST come from a cryptographic RNG. The previous implementation seeded
	// an FRandomStream from a time/cycle/PID/stack mix and drew all 32 bytes from
	// it — but FRandomStream is a 32-bit-state LCG (Seed = Seed*196314165 +
	// 907633515) with a single uint32 of state. Every byte of the "256-bit" token
	// was therefore a deterministic function of at most 2^32 states, and because
	// the seed was dominated by the wall clock, an attacker who knew editor start
	// time to within a second could collapse the search space by orders of
	// magnitude. This token is the sole credential guarding a remote-bindable
	// tool surface that can run console commands and invoke arbitrary UFunctions,
	// so a 32-bit secret is not acceptable.
	//
	// Primary source is the OS CSPRNG (BCryptGenRandom on Windows, /dev/urandom
	// on Mac/Linux — the plugin's only allow-listed platforms). If that fails we
	// fall back to SHA-256 hash-extraction over the multi-source mix, which at
	// least preserves the inputs' full width, and we log a warning so the
	// degradation is visible rather than silent.
	//
	// Token compare is constant-time and the auth check fails closed, so even a
	// pathological all-zero token is rejected by a required-mode check with no
	// matching expected token.
	uint8 Bytes[ByteLength];
	FMemory::Memzero(Bytes, sizeof(Bytes));

	if (!TryFillFromOsCsprng(Bytes, ByteLength))
	{
		UE_LOG(
			LogUnrealOpenMcp,
			Warning,
			TEXT("[Unreal Open MCP] OS cryptographic RNG unavailable; minting the bridge token via ")
			TEXT("SHA-256 hash extraction. Prefer authMode \"none\" on loopback over a remote bind ")
			TEXT("until this is resolved."));
		FillFromHashExtraction(Bytes, ByteLength);
	}

	// Hex-encode (lowercase, 2 chars per byte) — matches Unity's
	// bytes[i].ToString("x2").
	FString Out;
	Out.Reserve(HexLength);
	for (int32 i = 0; i < ByteLength; ++i)
	{
		Out += HexNibble(Bytes[i] >> 4);
		Out += HexNibble(Bytes[i] & 0x0F);
	}
	return Out;
}

bool FUnrealOpenMcpBridgeAuthToken::EqualsConstantTime(const FString& A, const FString& B)
{
	// Mirror Unity's EqualsConstantTime exactly: seed diff with the length XOR,
	// then fold a per-char XOR over maxLen. The loop always runs maxLen so
	// equal-length inputs do the same amount of work regardless of where they
	// differ (no early-exit timing leak). Different lengths return false (the
	// seed already set diff != 0) but still run the full loop.
	// Accumulate in a 32-bit width, NOT uint8.
	//
	// TCHAR is 16 or 32 bits wide. Masking each XOR to its low byte discarded
	// every difference in the upper bits, so the comparison was not injective and
	// reported DIFFERENT strings as equal: EqualsConstantTime("A", "Ł")
	// folded (0x41 ^ 0x141) & 0xFF == 0 with equal lengths and returned true. The
	// length seed had the same defect — lengths differing by exactly 0x100 (1 vs
	// 257) XOR to 0x100, masked to 0. A comparison primitive that accepts a
	// non-matching credential is wrong on its face, whatever the current
	// alphabet of the expected token happens to be.
	const int32 ALen = A.Len();
	const int32 BLen = B.Len();
	const int32 MaxLen = FMath::Max(ALen, BLen);

	uint32 Diff = static_cast<uint32>(ALen) ^ static_cast<uint32>(BLen);
	for (int32 i = 0; i < MaxLen; ++i)
	{
		TCHAR CA = 0;
		TCHAR CB = 0;
		if (i < ALen)
		{
			CA = A[i];
		}
		if (i < BLen)
		{
			CB = B[i];
		}
		Diff |= static_cast<uint32>(CA) ^ static_cast<uint32>(CB);
	}
	return Diff == 0;
}

FString FUnrealOpenMcpBridgeAuthToken::ExtractBearer(const FString& HeaderValue)
{
	if (HeaderValue.IsEmpty())
	{
		return FString();
	}

	const FString Trimmed = HeaderValue.TrimStartAndEnd();
	const int32 PrefixLen = FCString::Strlen(BearerPrefix);

	// Must be strictly longer than "Bearer " (need a token after it).
	if (Trimmed.Len() <= PrefixLen)
	{
		return FString();
	}

	// Case-insensitive scheme match on the "Bearer " prefix (incl. trailing
	// space).
	if (!Trimmed.Left(PrefixLen).Equals(BearerPrefix, ESearchCase::IgnoreCase))
	{
		return FString();
	}

	const FString Token = Trimmed.Mid(PrefixLen).TrimStartAndEnd();
	return Token;
}
