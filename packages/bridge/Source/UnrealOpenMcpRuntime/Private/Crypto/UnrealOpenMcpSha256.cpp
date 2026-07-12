// SHA-256 (FIPS 180-4).
// See header for why this is self-contained (FSHA1 is SHA-1; OpenSSL-backed
// FSHA256 is a heavy packaging dependency for hashing a project path).
#include "Crypto/UnrealOpenMcpSha256.h"

// SHA-256 round constants — first 32 bits of the fractional parts of the cube
// roots of the first 64 primes (FIPS 180-4 §4.2.2).
static const uint32 Sha256K[64] =
{
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
	0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
	0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
	0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
	0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
	0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
	0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
	0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32 RotateRight32(uint32 X, uint32 N)
{
	return (X >> N) | (X << (32u - N));
}

// Lowercase hex digit for a nibble in [0, 15]. Avoids the locale-dependent
// sprintf path and keeps the digest byte-for-byte identical to Node
// crypto.createHash('sha256').digest('hex') (lowercase).
static inline TCHAR HexDigit(uint8 Nibble)
{
	return (Nibble < 10u) ? static_cast<TCHAR>('0' + Nibble) : static_cast<TCHAR>('a' + Nibble - 10u);
}

FUnrealOpenMcpSha256::FUnrealOpenMcpSha256()
{
	// Initial hash values — first 32 bits of the fractional parts of the square
	// roots of the first 8 primes (FIPS 180-4 §5.3.3).
	State[0] = 0x6a09e667u;
	State[1] = 0xbb67ae85u;
	State[2] = 0x3c6ef372u;
	State[3] = 0xa54ff53au;
	State[4] = 0x510e527fu;
	State[5] = 0x9b05688cu;
	State[6] = 0x1f83d9abu;
	State[7] = 0x5be0cd19u;
	BitLength = 0;
	BufferLength = 0;
}

void FUnrealOpenMcpSha256::ProcessBlock(const uint8* Block)
{
	uint32 W[64];
	for (int32 i = 0; i < 16; ++i)
	{
		W[i] = (static_cast<uint32>(Block[i * 4 + 0]) << 24) |
		       (static_cast<uint32>(Block[i * 4 + 1]) << 16) |
		       (static_cast<uint32>(Block[i * 4 + 2]) << 8) |
		       (static_cast<uint32>(Block[i * 4 + 3]));
	}
	for (int32 i = 16; i < 64; ++i)
	{
		const uint32 S0 = RotateRight32(W[i - 15], 7) ^ RotateRight32(W[i - 15], 18) ^ (W[i - 15] >> 3);
		const uint32 S1 = RotateRight32(W[i - 2], 17) ^ RotateRight32(W[i - 2], 19) ^ (W[i - 2] >> 10);
		W[i] = W[i - 16] + S0 + W[i - 7] + S1;
	}

	uint32 A = State[0];
	uint32 B = State[1];
	uint32 C = State[2];
	uint32 D = State[3];
	uint32 E = State[4];
	uint32 F = State[5];
	uint32 G = State[6];
	uint32 H = State[7];

	for (int32 i = 0; i < 64; ++i)
	{
		const uint32 S1 = RotateRight32(E, 6) ^ RotateRight32(E, 11) ^ RotateRight32(E, 25);
		const uint32 Ch = (E & F) ^ ((~E) & G);
		const uint32 Temp1 = H + S1 + Ch + Sha256K[i] + W[i];
		const uint32 S0 = RotateRight32(A, 2) ^ RotateRight32(A, 13) ^ RotateRight32(A, 22);
		const uint32 Maj = (A & B) ^ (A & C) ^ (B & C);
		const uint32 Temp2 = S0 + Maj;

		H = G;
		G = F;
		F = E;
		E = D + Temp1;
		D = C;
		C = B;
		B = A;
		A = Temp1 + Temp2;
	}

	State[0] += A;
	State[1] += B;
	State[2] += C;
	State[3] += D;
	State[4] += E;
	State[5] += F;
	State[6] += G;
	State[7] += H;
}

void FUnrealOpenMcpSha256::Update(const uint8* Data, int32 NumBytes)
{
	if (Data == nullptr || NumBytes <= 0)
	{
		return;
	}

	BitLength += static_cast<uint64>(NumBytes) * 8u;

	// Top up the buffer with the remainder from a previous Update.
	if (BufferLength > 0)
	{
		const int32 Need = 64 - BufferLength;
		const int32 Take = FMath::Min(Need, NumBytes);
		FMemory::Memcpy(Buffer + BufferLength, Data, Take);
		BufferLength += Take;
		Data += Take;
		NumBytes -= Take;
		if (BufferLength == 64)
		{
			ProcessBlock(Buffer);
			BufferLength = 0;
		}
	}

	// Process full blocks straight from the input.
	while (NumBytes >= 64)
	{
		ProcessBlock(Data);
		Data += 64;
		NumBytes -= 64;
	}

	// Stash any tail for Final().
	if (NumBytes > 0)
	{
		FMemory::Memcpy(Buffer + BufferLength, Data, NumBytes);
		BufferLength += NumBytes;
	}
}

void FUnrealOpenMcpSha256::Final(uint8 OutDigest[32])
{
	// Pad: append 0x80, then zeros, then the 64-bit big-endian length so the
	// total is a multiple of 64 (FIPS 180-4 §5.1.1).
	Buffer[BufferLength++] = 0x80;

	if (BufferLength > 56)
	{
		while (BufferLength < 64)
		{
			Buffer[BufferLength++] = 0x00;
		}
		ProcessBlock(Buffer);
		BufferLength = 0;
	}

	while (BufferLength < 56)
	{
		Buffer[BufferLength++] = 0x00;
	}

	for (int32 i = 7; i >= 0; --i)
	{
		Buffer[BufferLength++] = static_cast<uint8>((BitLength >> (i * 8)) & 0xFFu);
	}

	ProcessBlock(Buffer);

	for (int32 i = 0; i < 8; ++i)
	{
		OutDigest[i * 4 + 0] = static_cast<uint8>((State[i] >> 24) & 0xFFu);
		OutDigest[i * 4 + 1] = static_cast<uint8>((State[i] >> 16) & 0xFFu);
		OutDigest[i * 4 + 2] = static_cast<uint8>((State[i] >> 8) & 0xFFu);
		OutDigest[i * 4 + 3] = static_cast<uint8>(State[i] & 0xFFu);
	}
}

void FUnrealOpenMcpSha256::HashString(const FString& Value, uint8 OutDigest[32])
{
	FUnrealOpenMcpSha256 Hash;
	const FTCHARToUTF8 Utf8(*Value);
	Hash.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Hash.Final(OutDigest);
}

FString FUnrealOpenMcpSha256::HexDigest(const FString& Value)
{
	uint8 Digest[32];
	HashString(Value, Digest);

	FString Out;
	Out.Reserve(64);
	for (int32 i = 0; i < 32; ++i)
	{
		Out += HexDigit(Digest[i] >> 4);
		Out += HexDigit(Digest[i] & 0x0Fu);
	}
	return Out;
}
