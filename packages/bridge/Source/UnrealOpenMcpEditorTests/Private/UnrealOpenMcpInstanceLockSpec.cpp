// Instance lock Automation specs (P1.4).
//
// Ports Unity Open MCP's BridgeInstanceLockTests
// (packages/bridge/Tests/Editor/Bridge/BridgeInstanceLockTests.cs) to Unreal.
// Lock I/O is sandboxed to a temp dir via the InstancesDirOverride parameter so
// specs never touch the real ~/.unreal-open-mcp/instances.
//
// We can't easily fake a different PID (Acquire writes the current editor's
// PID), so the stale-lock test plants a fake lock JSON with a guaranteed-dead
// PID before calling Acquire, then verifies it disappeared.
//
// authToken: the field is OMITTED from the JSON per the P1.4 plan (deferred to
// P5.6). Its absence is pinned below — when P5.6 adds the field, update this
// spec and the corresponding TS reader in the same task.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

#include "Bridge/UnrealOpenMcpBridgeInstanceLock.h"
#include "Bridge/UnrealOpenMcpInstancePortResolver.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpInstanceLockSpec,
	"UnrealOpenMcp.Bridge.InstanceLock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpInstanceLockSpec)

namespace
{
// Unique temp dir per spec run. Sandbox so specs never touch the real
// ~/.unreal-open-mcp/instances.
FString MakeTempInstancesDir()
{
	const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	return FPaths::Combine(
		FPaths::ConvertRelativePathToFull(FPaths::SystemTempDir()),
		TEXT("unreal-open-mcp-tests"),
		Guid);
}

bool ReadFile(const FString& Path, FString& OutContent)
{
	return FFileHelper::LoadFileToString(OutContent, *Path);
}

bool FileExists(const FString& Path)
{
	return IFileManager::Get().FileExists(*Path);
}

void RemoveDirRecursive(const FString& Path)
{
	if (!Path.IsEmpty())
	{
		IFileManager::Get().DeleteDirectory(*Path, /*bTree*/ true, /*bRequireExists*/ false);
	}
}
} // end anonymous namespace

void FUnrealOpenMcpInstanceLockSpec::Define()
{
	const FString TestProjectPath = TEXT("/test/MyGame");
	const FString OtherProjectPath = TEXT("/test/OtherGame");

	Describe("Acquire", [this, TestProjectPath]()
	{
		It("writes the lock file with the expected shape", [this, TestProjectPath]()
		{
			const FString TempDir = MakeTempInstancesDir();
			FUnrealOpenMcpBridgeInstanceLock Lock;
			Lock.Acquire(TestProjectPath, 22028, TEXT("0.0.1"), TEXT("5.8.0"), TempDir);

			TestTrue(TEXT("acquired"), Lock.IsAcquired());

			const FString LockPath = FUnrealOpenMcpInstancePortResolver::GetLockPath(TestProjectPath, TempDir);
			TestTrue(TEXT("lock file exists"), FileExists(LockPath));

			FString Json;
			ReadFile(LockPath, Json);

			// Required fields present. Field names mirror the TS-side
			// InstanceLock type in instance-discovery.ts.
			const TArray<FString> RequiredFields = {
				TEXT("\"pid\""), TEXT("\"port\""), TEXT("\"projectPath\""), TEXT("\"projectHash\""),
				TEXT("\"startedAt\""), TEXT("\"updatedAt\""), TEXT("\"heartbeatAt\""),
				TEXT("\"state\""), TEXT("\"isPlaying\""), TEXT("\"isCompiling\""),
				TEXT("\"bridgeVersion\""), TEXT("\"unrealVersion\"")
			};
			for (const FString& Field : RequiredFields)
			{
				TestTrue(
					FString::Printf(TEXT("has field %s"), *Field),
					Json.Contains(Field));
			}

			TestTrue(TEXT("port written"), Json.Contains(TEXT("\"port\":22028")));
			TestTrue(TEXT("projectPath written"), Json.Contains(TEXT("\"projectPath\":\"/test/MyGame\"")));
			TestTrue(TEXT("state idle"), Json.Contains(TEXT("\"state\":\"idle\"")));
			TestTrue(
				TEXT("projectHash matches resolver"),
				Json.Contains(FString::Printf(
					TEXT("\"projectHash\":\"%s\""),
					*FUnrealOpenMcpInstancePortResolver::ProjectHash(TestProjectPath))));

			Lock.Release();
			RemoveDirRecursive(TempDir);
		});

		It("is idempotent and overwrites the same project's lock", [this, TestProjectPath]()
		{
			const FString TempDir = MakeTempInstancesDir();
			FUnrealOpenMcpBridgeInstanceLock Lock;
			Lock.Acquire(TestProjectPath, 22028, TEXT("0.0.1"), TEXT("5.8.0"), TempDir);
			const FString LockPath = FUnrealOpenMcpInstancePortResolver::GetLockPath(TestProjectPath, TempDir);

			FString First;
			ReadFile(LockPath, First);

			// Re-acquire with a new port (simulates a bridge restart on the
			// same project). The lock should be replaced atomically.
			FUnrealOpenMcpBridgeInstanceLock Lock2;
			Lock2.Acquire(TestProjectPath, 22029, TEXT("0.0.1"), TEXT("5.8.0"), TempDir);

			FString Second;
			ReadFile(LockPath, Second);

			TestTrue(TEXT("second port reflected"), Second.Contains(TEXT("\"port\":22029")));
			TestNotEqual(TEXT("lock rewritten"), First, Second);

			Lock.Release();
			Lock2.Release();
			RemoveDirRecursive(TempDir);
		});

		// P1.4: authToken is deferred to P5.6. Pin its ABSENCE in the JSON so a
		// later phase adding it is a deliberate, visible change.
		It("omits the authToken field (P5.6 deferred)", [this, TestProjectPath]()
		{
			const FString TempDir = MakeTempInstancesDir();
			FUnrealOpenMcpBridgeInstanceLock Lock;
			Lock.Acquire(TestProjectPath, 22028, TEXT("0.0.1"), TEXT("5.8.0"), TempDir);

			const FString LockPath = FUnrealOpenMcpInstancePortResolver::GetLockPath(TestProjectPath, TempDir);
			FString Json;
			ReadFile(LockPath, Json);

			TestFalse(
				TEXT("authToken NOT present in JSON (P5.6 deferred)"),
				Json.Contains(TEXT("\"authToken\"")));

			Lock.Release();
			RemoveDirRecursive(TempDir);
		});
	});

	Describe("UpdateState", [this, TestProjectPath]()
	{
		It("rewrites the lock with fresh state", [this, TestProjectPath]()
		{
			const FString TempDir = MakeTempInstancesDir();
			FUnrealOpenMcpBridgeInstanceLock Lock;
			Lock.Acquire(TestProjectPath, 22028, TEXT("0.0.1"), TEXT("5.8.0"), TempDir);
			Lock.UpdateState(FUnrealOpenMcpBridgeInstanceLock::StateCompiling(), false, true);

			const FString LockPath = FUnrealOpenMcpInstancePortResolver::GetLockPath(TestProjectPath, TempDir);
			FString Json;
			ReadFile(LockPath, Json);
			TestTrue(TEXT("state=compiling"), Json.Contains(TEXT("\"state\":\"compiling\"")));
			TestTrue(TEXT("isCompiling=true"), Json.Contains(TEXT("\"isCompiling\":true")));

			Lock.Release();
			RemoveDirRecursive(TempDir);
		});

		It("is a no-op before Acquire", [this, TestProjectPath]()
		{
			const FString TempDir = MakeTempInstancesDir();
			FUnrealOpenMcpBridgeInstanceLock Lock;
			// No Acquire — UpdateState must not write anything.
			Lock.UpdateState(FUnrealOpenMcpBridgeInstanceLock::StateIdle(), false, false);

			const FString LockPath = FUnrealOpenMcpInstancePortResolver::GetLockPath(TestProjectPath, TempDir);
			TestFalse(TEXT("no lock file before Acquire"), FileExists(LockPath));
			RemoveDirRecursive(TempDir);
		});
	});

	Describe("Release", [this, TestProjectPath]()
	{
		It("deletes the lock", [this, TestProjectPath]()
		{
			const FString TempDir = MakeTempInstancesDir();
			FUnrealOpenMcpBridgeInstanceLock Lock;
			Lock.Acquire(TestProjectPath, 22028, TEXT("0.0.1"), TEXT("5.8.0"), TempDir);
			const FString LockPath = FUnrealOpenMcpInstancePortResolver::GetLockPath(TestProjectPath, TempDir);
			TestTrue(TEXT("exists before Release"), FileExists(LockPath));

			Lock.Release();
			TestFalse(TEXT("deleted after Release"), FileExists(LockPath));
			TestFalse(TEXT("not acquired after Release"), Lock.IsAcquired());
			RemoveDirRecursive(TempDir);
		});

		It("is idempotent", [this, TestProjectPath]()
		{
			const FString TempDir = MakeTempInstancesDir();
			FUnrealOpenMcpBridgeInstanceLock Lock;
			Lock.Acquire(TestProjectPath, 22028, TEXT("0.0.1"), TEXT("5.8.0"), TempDir);
			Lock.Release();
			// Second release is a no-op (already not acquired).
			Lock.Release();
			TestFalse(TEXT("still not acquired"), Lock.IsAcquired());
			RemoveDirRecursive(TempDir);
		});
	});

	Describe("ReadCurrentJson", [this, TestProjectPath]()
	{
		It("returns the lock content when acquired", [this, TestProjectPath]()
		{
			const FString TempDir = MakeTempInstancesDir();
			FUnrealOpenMcpBridgeInstanceLock Lock;
			Lock.Acquire(TestProjectPath, 22028, TEXT("0.0.1"), TEXT("5.8.0"), TempDir);
			const FString Json = Lock.ReadCurrentJson();
			TestTrue(TEXT("non-empty"), !Json.IsEmpty());
			TestTrue(TEXT("has port"), Json.Contains(TEXT("\"port\":22028")));
			Lock.Release();
			RemoveDirRecursive(TempDir);
		});

		It("returns empty when not acquired", [this]()
		{
			FUnrealOpenMcpBridgeInstanceLock Lock;
			TestTrue(TEXT("empty when not acquired"), Lock.ReadCurrentJson().IsEmpty());
		});
	});

	Describe("Stale-lock cleanup", [this, TestProjectPath, OtherProjectPath]()
	{
		It("deletes a stale lock for a dead PID", [this, TestProjectPath, OtherProjectPath]()
		{
			const FString TempDir = MakeTempInstancesDir();
			// Plant a lock for a DIFFERENT project with a guaranteed-dead PID.
			const FString OtherPath = FUnrealOpenMcpInstancePortResolver::GetLockPath(OtherProjectPath, TempDir);
			const FString FakeJson = FString::Printf(
				TEXT("{\"pid\":999999999,\"port\":25000,\"projectPath\":\"%s\",\"state\":\"idle\"}"),
				*OtherProjectPath);
			FFileHelper::SaveStringToFile(FakeJson, *OtherPath);
			TestTrue(TEXT("pre-condition: stale lock exists"), FileExists(OtherPath));

			FUnrealOpenMcpBridgeInstanceLock Lock;
			Lock.Acquire(TestProjectPath, 22028, TEXT("0.0.1"), TEXT("5.8.0"), TempDir);

			TestFalse(TEXT("stale lock cleaned up"), FileExists(OtherPath));

			Lock.Release();
			RemoveDirRecursive(TempDir);
		});

		It("leaves a lock for a live PID alone", [this, TestProjectPath, OtherProjectPath]()
		{
			const FString TempDir = MakeTempInstancesDir();
			// Plant a lock for a different project using OUR own PID (the test
			// runner is guaranteed alive).
			const FString OtherPath = FUnrealOpenMcpInstancePortResolver::GetLockPath(OtherProjectPath, TempDir);
			const uint32 LivePid = FPlatformProcess::GetCurrentProcessId();
			const FString FakeJson = FString::Printf(
				TEXT("{\"pid\":%u,\"port\":25000,\"projectPath\":\"%s\",\"state\":\"idle\"}"),
				LivePid, *OtherProjectPath);
			FFileHelper::SaveStringToFile(FakeJson, *OtherPath);

			FUnrealOpenMcpBridgeInstanceLock Lock;
			Lock.Acquire(TestProjectPath, 22028, TEXT("0.0.1"), TEXT("5.8.0"), TempDir);

			TestTrue(TEXT("live-PID lock NOT cleaned up"), FileExists(OtherPath));

			Lock.Release();
			RemoveDirRecursive(TempDir);
		});

		It("leaves a malformed lock (no pid) alone", [this, TestProjectPath, OtherProjectPath]()
		{
			const FString TempDir = MakeTempInstancesDir();
			const FString OtherPath = FUnrealOpenMcpInstancePortResolver::GetLockPath(OtherProjectPath, TempDir);
			FFileHelper::SaveStringToFile(TEXT("{\"notPid\":\"oops\"}"), *OtherPath);

			FUnrealOpenMcpBridgeInstanceLock Lock;
			Lock.Acquire(TestProjectPath, 22028, TEXT("0.0.1"), TEXT("5.8.0"), TempDir);

			TestTrue(TEXT("malformed lock left in place"), FileExists(OtherPath));

			Lock.Release();
			RemoveDirRecursive(TempDir);
		});
	});

	Describe("Atomic write", [this, TestProjectPath]()
	{
		It("leaves no .tmp file behind after UpdateState", [this, TestProjectPath]()
		{
			const FString TempDir = MakeTempInstancesDir();
			FUnrealOpenMcpBridgeInstanceLock Lock;
			Lock.Acquire(TestProjectPath, 22028, TEXT("0.0.1"), TEXT("5.8.0"), TempDir);
			Lock.UpdateState(FUnrealOpenMcpBridgeInstanceLock::StatePlaying(), true, false);

			// Scan for leftover .tmp.* files matching the lock name.
			TArray<FString> Files;
			IFileManager::Get().FindFiles(Files, *TempDir, TEXT(".tmp.*"));
			TestEqual(TEXT("no leftover tmp files"), Files.Num(), 0);

			Lock.Release();
			RemoveDirRecursive(TempDir);
		});
	});

	Describe("TryParseSnapshot", [this]()
	{
		It("returns invalid for null/empty input", [this]()
		{
			TestFalse(TEXT("null → invalid"), FUnrealOpenMcpBridgeInstanceLock::TryParseSnapshot(FString()).bValid);
			TestFalse(TEXT("empty → invalid"), FUnrealOpenMcpBridgeInstanceLock::TryParseSnapshot(TEXT("")).bValid);
		});

		It("returns invalid when no pid", [this]()
		{
			const FString Json = TEXT("{\"port\":22028,\"state\":\"idle\"}");
			TestFalse(TEXT("no pid → invalid"), FUnrealOpenMcpBridgeInstanceLock::TryParseSnapshot(Json).bValid);
		});

		It("extracts all fields from a full payload", [this]()
		{
			const FString Json = TEXT(
				"{\"pid\":12345,\"port\":22028,\"projectPath\":\"/p\","
				"\"state\":\"compiling\",\"updatedAt\":\"2026-06-26T10:00:00Z\","
				"\"heartbeatAt\":\"2026-06-26T10:00:01Z\"}");
			const auto Snap = FUnrealOpenMcpBridgeInstanceLock::TryParseSnapshot(Json);
			TestTrue(TEXT("valid"), Snap.bValid);
			TestEqual(TEXT("pid"), Snap.Pid, 12345);
			TestEqual(TEXT("port"), Snap.Port, 22028);
			TestEqual(TEXT("state"), Snap.State, FString(TEXT("compiling")));
			TestEqual(TEXT("updatedAt"), Snap.UpdatedAt, FString(TEXT("2026-06-26T10:00:00Z")));
			TestEqual(TEXT("heartbeatAt"), Snap.HeartbeatAt, FString(TEXT("2026-06-26T10:00:01Z")));
		});

		It("unescapes string values", [this]()
		{
			const FString Json = TEXT("{\"pid\":1,\"state\":\"a\\\"b\"}");
			const auto Snap = FUnrealOpenMcpBridgeInstanceLock::TryParseSnapshot(Json);
			TestTrue(TEXT("valid"), Snap.bValid);
			TestEqual(TEXT("escaped state"), Snap.State, FString(TEXT("a\"b")));
		});

		It("returns defaults for missing optional fields", [this]()
		{
			const auto Snap = FUnrealOpenMcpBridgeInstanceLock::TryParseSnapshot(TEXT("{\"pid\":7}"));
			TestTrue(TEXT("valid"), Snap.bValid);
			TestEqual(TEXT("pid"), Snap.Pid, 7);
			TestEqual(TEXT("port absent → 0"), Snap.Port, 0);
			TestTrue(TEXT("state absent → empty"), Snap.State.IsEmpty());
		});

		It("round-trips a real acquired lock", [this, TestProjectPath]()
		{
			const FString TempDir = MakeTempInstancesDir();
			FUnrealOpenMcpBridgeInstanceLock Lock;
			Lock.Acquire(TestProjectPath, 22028, TEXT("0.0.1"), TEXT("5.8.0"), TempDir);
			const FString Json = Lock.ReadCurrentJson();
			TestTrue(TEXT("json present"), !Json.IsEmpty());

			const auto Snap = FUnrealOpenMcpBridgeInstanceLock::TryParseSnapshot(Json);
			TestTrue(TEXT("valid"), Snap.bValid);
			TestEqual(TEXT("port"), Snap.Port, 22028);
			TestEqual(TEXT("pid is this process"),
				Snap.Pid,
				static_cast<int32>(FPlatformProcess::GetCurrentProcessId()));
			TestEqual(TEXT("state idle"), Snap.State, FString(FUnrealOpenMcpBridgeInstanceLock::StateIdle()));

			Lock.Release();
			RemoveDirRecursive(TempDir);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
