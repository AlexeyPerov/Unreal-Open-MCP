// Instance lock + heartbeat file.
// See header for the lifecycle + cross-side parity contract.
#include "Bridge/UnrealOpenMcpBridgeInstanceLock.h"

#include "Bridge/UnrealOpenMcpBridgeJson.h"
#include "Bridge/UnrealOpenMcpInstancePortResolver.h"
#include "UnrealOpenMcpLog.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
// ISO-8601 UTC with milliseconds + Z: yyyy-MM-dd'T'HH:mm:ss.fffZ. Mirrors
// Unity's BridgeInstanceLock.IsoUtc format and the TS reader's expectations so
// instance-discovery.ts parses the timestamps without timezone conversion.
FString FormatIsoUtc(const FDateTime& Utc)
{
	return FString::Printf(
		TEXT("%04d-%02d-%02dT%02d:%02d:%02d.%03dZ"),
		Utc.GetYear(), Utc.GetMonth(), Utc.GetDay(),
		Utc.GetHour(), Utc.GetMinute(), Utc.GetSecond(),
		Utc.GetMillisecond());
}
} // end anonymous namespace

FUnrealOpenMcpBridgeInstanceLock::~FUnrealOpenMcpBridgeInstanceLock()
{
	// Defensive Release — module destruct order can race ShutdownModule.
	Release();
}

void FUnrealOpenMcpBridgeInstanceLock::Acquire(
	const FString& ProjectPath,
	int32 Port,
	const FString& BridgeVersion,
	const FString& UnrealVersion,
	const FString& InstancesDirOverride)
{
	if (ProjectPath.IsEmpty())
	{
		UE_LOG(
			LogUnrealOpenMcp,
			Warning,
			TEXT("[Unreal Open MCP] no project path available; skipping instance lock acquire."));
		return;
	}

	InstancesDirOverrideForLock = InstancesDirOverride;

	EnsureInstancesDir();
	SweepStaleLocks();

	AcquiredProjectPath = ProjectPath;
	AcquiredPort = Port;
	AcquiredProjectHash = FUnrealOpenMcpInstancePortResolver::ProjectHash(ProjectPath);
	Pid = FPlatformProcess::GetCurrentProcessId();
	StartedAt = FDateTime::UtcNow();
	BridgeVersionForLock = BridgeVersion;
	UnrealVersionForLock = UnrealVersion;

	WriteLock(StateIdle(), /*bIsPlaying*/ false, /*bIsCompiling*/ false, FDateTime::UtcNow());
	bAcquired = true;
}

void FUnrealOpenMcpBridgeInstanceLock::UpdateState(const FString& State, bool bIsPlaying, bool bIsCompiling)
{
	if (!bAcquired)
	{
		return;
	}
	WriteLock(State.IsEmpty() ? FString(StateIdle()) : State, bIsPlaying, bIsCompiling, FDateTime::UtcNow());
}

void FUnrealOpenMcpBridgeInstanceLock::Release()
{
	if (!bAcquired)
	{
		return;
	}
	const FString Path = GetAcquiredLockPath();
	if (!Path.IsEmpty())
	{
		IFileManager::Get().Delete(*Path);
	}
	bAcquired = false;
}

FString FUnrealOpenMcpBridgeInstanceLock::ReadCurrentJson() const
{
	if (!bAcquired)
	{
		return FString();
	}
	const FString Path = GetAcquiredLockPath();
	if (Path.IsEmpty())
	{
		return FString();
	}
	FString Out;
	if (FFileHelper::LoadFileToString(Out, *Path))
	{
		return Out;
	}
	return FString();
}

FString FUnrealOpenMcpBridgeInstanceLock::GetAcquiredLockPath() const
{
	if (AcquiredProjectPath.IsEmpty())
	{
		return FString();
	}
	return FUnrealOpenMcpInstancePortResolver::GetLockPath(AcquiredProjectPath, InstancesDirOverrideForLock);
}

void FUnrealOpenMcpBridgeInstanceLock::EnsureInstancesDir() const
{
	const FString Dir = FUnrealOpenMcpInstancePortResolver::GetInstancesDir(InstancesDirOverrideForLock);
	IFileManager::Get().MakeDirectory(*Dir, /*bTree*/ true);
}

void FUnrealOpenMcpBridgeInstanceLock::SweepStaleLocks() const
{
	const FString Dir = FUnrealOpenMcpInstancePortResolver::GetInstancesDir(InstancesDirOverrideForLock);
	if (!IFileManager::Get().DirectoryExists(*Dir))
	{
		return;
	}

	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *Dir, TEXT(".json"));
	if (Files.Num() == 0)
	{
		return;
	}

	for (const FString& File : Files)
	{
		const FString FullPath = FPaths::Combine(Dir, File);

		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *FullPath))
		{
			continue;
		}

		const int32 LockPid = ExtractInt(Json, TEXT("pid"));
		if (LockPid <= 0)
		{
			// Malformed (no parseable pid) — leave it alone. It may belong to
			// another tool's instance file.
			continue;
		}

		if (IsPidAlive(static_cast<uint32>(LockPid)))
		{
			continue;
		}

		// Dead PID — delete the stale lock (best-effort).
		IFileManager::Get().Delete(*FullPath);
	}
}

void FUnrealOpenMcpBridgeInstanceLock::WriteLock(const FString& State, bool bIsPlaying, bool bIsCompiling, const FDateTime& Now)
{
	const FString Path = GetAcquiredLockPath();
	if (Path.IsEmpty())
	{
		return;
	}

	const FString Dir = FPaths::GetPath(Path);
	if (!Dir.IsEmpty() && !IFileManager::Get().DirectoryExists(*Dir))
	{
		IFileManager::Get().MakeDirectory(*Dir, /*bTree*/ true);
	}

	const FString Json = BuildJson(State, bIsPlaying, bIsCompiling, Now);

	// Atomic write: .tmp.<pid> then rename. Per-PID tmp avoids collisions when
	// two instances briefly race (the second rename wins; both are valid).
	const FString Tmp = FString::Printf(TEXT("%s.tmp.%u"), *Path, Pid);

	// Write as UTF-8 (no BOM). The TS reader uses JSON.parse on UTF-8 bytes.
	const FTCHARToUTF8 Utf8(*Json);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	if (!FFileHelper::SaveArrayToFile(Bytes, *Tmp))
	{
		return;
	}

	// Atomic replace. IFileManager::Move with bAtomic uses rename() on Unix
	// (atomic on the same filesystem) and ReplaceFile on Windows.
	IFileManager::Get().Move(*Path, *Tmp, /*bReplace*/ true, /*bNoOverwriteReadonly*/ false, /*bAtomic*/ true);
}

FString FUnrealOpenMcpBridgeInstanceLock::BuildJson(const FString& State, bool bIsPlaying, bool bIsCompiling, const FDateTime& Now) const
{
	const FString StartedAtIso = FormatIsoUtc(StartedAt);
	const FString NowIso = FormatIsoUtc(Now);

	// Pinned field order — mirrors Unity's BridgeInstanceLock.BuildJson minus
	// authToken (deferred to P5.6). Field names are identical so the TS reader
	// (instance-discovery.ts InstanceLock) parses without modification.
	//
	// Uses the shared FUnrealOpenMcpBridgeJson::AppendJsonString appender for
	// every string value per packages/bridge/AGENTS.md §Transport (no inline
	// escape reimplementation).
	FString Out;
	Out.Reserve(512);
	Out += TEXT('{');

	// pid
	Out += TEXT("\"pid\":");
	Out += FString::FromInt(static_cast<int32>(Pid));

	// port
	Out += TEXT(",\"port\":");
	Out += FString::FromInt(AcquiredPort);

	// authToken intentionally OMITTED (P5.6 deferred per P1.4 plan). When P5.6
	// adds it, insert here (between port and projectPath) so the TS reader's
	// optional-field tolerance is unchanged. Absence is pinned in the spec.

	// projectPath
	Out += TEXT(",\"projectPath\":");
	FUnrealOpenMcpBridgeJson::AppendJsonString(Out, AcquiredProjectPath);

	// projectHash
	Out += TEXT(",\"projectHash\":");
	FUnrealOpenMcpBridgeJson::AppendJsonString(Out, AcquiredProjectHash);

	// startedAt
	Out += TEXT(",\"startedAt\":");
	FUnrealOpenMcpBridgeJson::AppendJsonString(Out, StartedAtIso);

	// updatedAt
	Out += TEXT(",\"updatedAt\":");
	FUnrealOpenMcpBridgeJson::AppendJsonString(Out, NowIso);

	// heartbeatAt
	Out += TEXT(",\"heartbeatAt\":");
	FUnrealOpenMcpBridgeJson::AppendJsonString(Out, NowIso);

	// state
	Out += TEXT(",\"state\":");
	const FString& SafeState = State.IsEmpty() ? FString(StateIdle()) : State;
	FUnrealOpenMcpBridgeJson::AppendJsonString(Out, SafeState);

	// isPlaying
	Out += TEXT(",\"isPlaying\":");
	Out += bIsPlaying ? TEXT("true") : TEXT("false");

	// isCompiling
	Out += TEXT(",\"isCompiling\":");
	Out += bIsCompiling ? TEXT("true") : TEXT("false");

	// bridgeVersion
	Out += TEXT(",\"bridgeVersion\":");
	FUnrealOpenMcpBridgeJson::AppendJsonString(Out, BridgeVersionForLock);

	// unrealVersion
	Out += TEXT(",\"unrealVersion\":");
	FUnrealOpenMcpBridgeJson::AppendJsonString(Out, UnrealVersionForLock);

	Out += TEXT('}');
	return Out;
}

bool FUnrealOpenMcpBridgeInstanceLock::IsPidAlive(uint32 PidToCheck)
{
	// kill -0 equivalent. Mirrors Unity's IsPidAlive.
	//
	// Note: Unity treats an access-denied Win32Exception as "alive" so it never
	// deletes a lock for a process it can't introspect. FPlatformProcess::
	// GetProcessIsAlive uses kill(pid, 0) on Unix — EPERM (access denied) makes
	// it return false. This is a narrow theoretical divergence from Unity's
	// conservative guard, but for our use case (an editor cleaning up its own
	// stale locks — always same-user processes) EPERM essentially never occurs.
	return FPlatformProcess::GetProcessIsAlive(static_cast<int32>(PidToCheck));
}

int32 FUnrealOpenMcpBridgeInstanceLock::ExtractInt(const FString& Json, const FString& Key)
{
	const FString QuotedKey = TEXT("\"") + Key + TEXT("\"");
	const int32 Idx = Json.Find(*QuotedKey, ESearchCase::CaseSensitive, ESearchDir::FromStart);
	if (Idx == INDEX_NONE)
	{
		return -1;
	}
	const int32 Colon = Json.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Idx + QuotedKey.Len());
	if (Colon == INDEX_NONE)
	{
		return -1;
	}
	int32 Start = Colon + 1;
	while (Start < Json.Len() && (Json[Start] == TEXT(' ') || Json[Start] == TEXT('\t')))
	{
		++Start;
	}
	int32 End = Start;
	while (End < Json.Len() && Json[End] >= TEXT('0') && Json[End] <= TEXT('9'))
	{
		++End;
	}
	if (End == Start)
	{
		return -1;
	}
	return FCString::Atoi(*Json.Mid(Start, End - Start));
}

FString FUnrealOpenMcpBridgeInstanceLock::ExtractString(const FString& Json, const FString& Key)
{
	const FString QuotedKey = TEXT("\"") + Key + TEXT("\"");
	const int32 Idx = Json.Find(*QuotedKey, ESearchCase::CaseSensitive, ESearchDir::FromStart);
	if (Idx == INDEX_NONE)
	{
		return FString();
	}
	const int32 Colon = Json.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Idx + QuotedKey.Len());
	if (Colon == INDEX_NONE)
	{
		return FString();
	}
	int32 Start = Colon + 1;
	while (Start < Json.Len() && (Json[Start] == TEXT(' ') || Json[Start] == TEXT('\t')))
	{
		++Start;
	}
	if (Start >= Json.Len() || Json[Start] != TEXT('"'))
	{
		return FString();
	}
	++Start; // skip opening quote

	FString Out;
	int32 i = Start;
	while (i < Json.Len())
	{
		const TCHAR c = Json[i];
		if (c == TEXT('"'))
		{
			return Out;
		}
		if (c == TEXT('\\') && i + 1 < Json.Len())
		{
			const TCHAR Next = Json[i + 1];
			switch (Next)
			{
				case TEXT('"'): Out += TEXT('"'); i += 2; continue;
				case TEXT('\\'): Out += TEXT('\\'); i += 2; continue;
				case TEXT('/'): Out += TEXT('/'); i += 2; continue;
				case TEXT('b'): Out += TEXT('\b'); i += 2; continue;
				case TEXT('f'): Out += TEXT('\f'); i += 2; continue;
				case TEXT('n'): Out += TEXT('\n'); i += 2; continue;
				case TEXT('r'): Out += TEXT('\r'); i += 2; continue;
				case TEXT('t'): Out += TEXT('\t'); i += 2; continue;
				case TEXT('u'):
					if (i + 5 < Json.Len())
					{
						const FString Hex = Json.Mid(i + 2, 4);
						const uint32 Code = static_cast<uint32>(FCString::Strtoi(*Hex, nullptr, 16));
						Out += static_cast<TCHAR>(Code);
						i += 6;
						continue;
					}
					break;
				default: break;
			}
		}
		Out += c;
		++i;
	}
	return Out; // unterminated — return what we have
}

FUnrealOpenMcpBridgeInstanceLock::FLockSnapshot FUnrealOpenMcpBridgeInstanceLock::TryParseSnapshot(const FString& Json)
{
	FLockSnapshot Snapshot;
	if (Json.IsEmpty())
	{
		return Snapshot;
	}

	Snapshot.Pid = ExtractInt(Json, TEXT("pid"));
	Snapshot.Port = ExtractInt(Json, TEXT("port"));
	Snapshot.State = ExtractString(Json, TEXT("state"));
	Snapshot.UpdatedAt = ExtractString(Json, TEXT("updatedAt"));
	Snapshot.HeartbeatAt = ExtractString(Json, TEXT("heartbeatAt"));
	// pid is the minimum signal that this is a real lock payload.
	Snapshot.bValid = Snapshot.Pid > 0;
	// A missing port should read as 0 (the natural absent sentinel for a port
	// number), not -1 which ExtractInt returns for absent keys.
	if (Snapshot.Port < 0)
	{
		Snapshot.Port = 0;
	}
	return Snapshot;
}
