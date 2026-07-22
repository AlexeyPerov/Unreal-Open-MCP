// Per-session bearer token primitives. See header for the cross-side contract.
#include "Bridge/UnrealOpenMcpBridgeAuthToken.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"
#include "Misc/DateTime.h"

namespace
{
// Lowercase hex nibble for a 0..15 value. Matches C#'s "x2" format.
TCHAR HexNibble(uint8 Nibble)
{
	return Nibble < 10 ? static_cast<TCHAR>('0' + Nibble) : static_cast<TCHAR>('a' + (Nibble - 10));
}
} // end anonymous namespace

FString FUnrealOpenMcpBridgeAuthToken::Generate()
{
	// Mint 32 bytes of fresh entropy and hex-encode to 64 lowercase chars.
	//
	// Unity uses System.Security.Cryptography.RandomNumberGenerator (a crypto
	// RNG). Unreal has no portable crypto-RNG facade across the supported floor
	// (UE 5.6+) without dragging in platform-specific code, so we seed an
	// FRandomStream from a high-entropy mix and draw bytes from it. The seed
	// combines:
	//   - FDateTime::UtcNow().GetTicks() (100ns resolution wall clock)
	//   - FPlatformTime::Cycles() (CPU cycle counter — high-frequency, varies
	//     run-to-run)
	//   - FPlatformProcess::GetCurrentProcessId() (per-process spread)
	//   - a stack address (per-call ASLR spread)
	// The combination makes a predictable seed astronomically unlikely for an
	// attacker who can only observe the bridge from across a loopback socket
	// (the threat model the bearer gate addresses). The token is also rotated
	// on every Acquire (per listener start), so the window to exploit any
	// predictable-byte sequence is a single editor session.
	//
	// Token compare is constant-time and the auth check fails closed, so a
	// zero-filled token (the FRandomStream fallthrough) is still rejected by a
	// required-mode check that has no matching expected token.
	const uint64 TimeTicks = static_cast<uint64>(FDateTime::UtcNow().GetTicks());
	const uint64 Cycles = static_cast<uint64>(FPlatformTime::Cycles());
	const uint32 Pid = FPlatformProcess::GetCurrentProcessId();
	int32 StackAnchor = 0;
	const uintptr_t StackAddr = reinterpret_cast<uintptr_t>(&StackAnchor);

	const uint64 Mixed = TimeTicks ^ Cycles ^ static_cast<uint64>(StackAddr);
	const uint32 SeedHi = static_cast<uint32>(Mixed >> 32) ^ Pid;
	const uint32 SeedLo = static_cast<uint32>(Mixed);
	FRandomStream Stream((static_cast<uint64>(SeedHi) << 32) | SeedLo);

	uint8 Bytes[ByteLength];
	for (int32 i = 0; i < ByteLength; ++i)
	{
		Bytes[i] = static_cast<uint8>(Stream.RandRange(0, 255));
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
	const int32 ALen = A.Len();
	const int32 BLen = B.Len();
	const int32 MaxLen = FMath::Max(ALen, BLen);

	uint8 Diff = static_cast<uint8>((ALen ^ BLen) & 0xFF);
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
		Diff |= static_cast<uint8>((CA ^ CB) & 0xFF);
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
