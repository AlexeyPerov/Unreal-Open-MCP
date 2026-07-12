// Per-project deterministic port + instance-lock path resolver.
// See header for the cross-side parity contract (Unity + TS mirror).
#include "Bridge/UnrealOpenMcpInstancePortResolver.h"

#include "Crypto/UnrealOpenMcpSha256.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

FString FUnrealOpenMcpInstancePortResolver::NormalizePath(const FString& ProjectPath)
{
	if (ProjectPath.IsEmpty())
	{
		return FString();
	}

	FString Normalized = ProjectPath;
	Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));

	// Trim trailing slashes; keep a lone "/" root.
	while (Normalized.Len() > 1 && Normalized.EndsWith(TEXT("/")))
	{
		Normalized = Normalized.LeftChop(1);
	}
	return Normalized;
}

FString FUnrealOpenMcpInstancePortResolver::ProjectHash(const FString& ProjectPath)
{
	const FString Normalized = NormalizePath(ProjectPath);
	return FUnrealOpenMcpSha256::HexDigest(Normalized);
}

int32 FUnrealOpenMcpInstancePortResolver::ComputePort(const FString& ProjectPath)
{
	const FString HashHex = ProjectHash(ProjectPath);

	// First 16 hex chars = first 8 bytes, parsed as a big-endian uint64. Keeps
	// the modulo inside uint64 range so C++, C#, and TS agree exactly. Mirrors
	// InstancePortResolver.ComputePort and instance-discovery.ts computePort.
	// Manual parse (not FParse::HexNumber64) keeps this version-independent —
	// the hex is already validated (lowercase [0-9a-f]) by HexDigest.
	const FString Prefix = HashHex.Left(16);
	uint64 Value = 0;
	for (int32 i = 0; i < Prefix.Len(); ++i)
	{
		const TCHAR C = Prefix[i];
		uint8 Digit = 0;
		if (C >= TEXT('0') && C <= TEXT('9'))
		{
			Digit = static_cast<uint8>(C - TEXT('0'));
		}
		else if (C >= TEXT('a') && C <= TEXT('f'))
		{
			Digit = static_cast<uint8>(10 + (C - TEXT('a')));
		}
		else if (C >= TEXT('A') && C <= TEXT('F'))
		{
			Digit = static_cast<uint8>(10 + (C - TEXT('A')));
		}
		Value = (Value << 4) | Digit;
	}
	return PortRangeStart + static_cast<int32>(Value % static_cast<uint64>(PortRangeSize));
}

int32 FUnrealOpenMcpInstancePortResolver::ResolvePort(
	const FString& ProjectPath,
	const TOptional<int32>& EnvPort,
	const TOptional<int32>& CliPort)
{
	// Env > CLI > hash. Invalid values fall through (the caller is responsible
	// only for reading, not validating — IsValidPort is checked here).
	if (EnvPort.IsSet() && IsValidPort(EnvPort.GetValue()))
	{
		return EnvPort.GetValue();
	}
	if (CliPort.IsSet() && IsValidPort(CliPort.GetValue()))
	{
		return CliPort.GetValue();
	}
	return ComputePort(ProjectPath);
}

FString FUnrealOpenMcpInstancePortResolver::GetInstancesDir(const FString& Override)
{
	if (!Override.IsEmpty())
	{
		// Test-only hook — sandbox lock I/O out of the real home dir.
		return Override;
	}

	const FString Home = FPlatformProcess::UserDir();
	// FPlatformProcess::UserDir() returns the home dir with a trailing slash on
	// most platforms — trim it so the join does not double up the separator.
	FString TrimmedHome = Home;
	while (TrimmedHome.EndsWith(TEXT("/")) || TrimmedHome.EndsWith(TEXT("\\")))
	{
		TrimmedHome = TrimmedHome.LeftChop(1);
	}
	return FPaths::Combine(TrimmedHome, SettingsDirName, InstancesSubdir);
}

FString FUnrealOpenMcpInstancePortResolver::GetLockPath(const FString& ProjectPath, const FString& InstancesDirOverride)
{
	if (ProjectPath.IsEmpty())
	{
		return FString();
	}
	const FString Dir = GetInstancesDir(InstancesDirOverride);
	const FString Hash = ProjectHash(ProjectPath);
	return FPaths::Combine(Dir, Hash + TEXT(".json"));
}
