// FUnrealOpenMcpFixRollback Automation specs (P3.7).
//
// Pins the snapshot/restore helper used by FUnrealOpenMcpApplyFixGateRunner.
// Uses the project's temp directory for fixture files so the spec is
// hermetic — no fixture content needs to exist in the project itself.
//
// Covered:
//   - Snapshot of an existing file → Restore writes the original bytes back.
//   - Snapshot of a non-existent file → Restore deletes the file the fix
//     created (the create-case rollback).
//   - HasSnapshot / Discard lifecycle.
//   - De-duplication when the same path is snapshotted twice.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Gate/UnrealOpenMcpFixRollback.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/IPlatformFile.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpFixRollbackSpec,
	"UnrealOpenMcp.Gate.FixRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpFixRollbackSpec)

namespace
{
	// Build a unique fixture path under the system temp dir. The path is
	// returned in absolute form so the rollback helper does not need to
	// resolve it.
	FString MakeFixturePath(const FString& Suffix)
	{
		const FString Base = FPaths::ConvertRelativePathToFull(
			FPaths::CreateTempFilename(*FPaths::SystemTempDir(), TEXT("UOMFixRollback_"), TEXT(".bin")));
		// CreateTempFilename already returns a unique path; the caller owns
		// creating the file. We just need a stable unique absolute path.
		(void)Suffix;
		return Base;
	}

	bool WriteFile(const FString& Path, const FString& Content)
	{
		return FFileHelper::SaveStringToFile(Content, *Path);
	}

	bool FileExists(const FString& Path)
	{
		return FPaths::FileExists(Path);
	}

	bool ReadFile(const FString& Path, FString& OutContent)
	{
		return FFileHelper::LoadFileToString(OutContent, *Path);
	}

	void DeleteFileIfExists(const FString& Path)
	{
		if (FPaths::FileExists(Path))
		{
			IFileManager::Get().Delete(*Path);
		}
	}
} // namespace

void FUnrealOpenMcpFixRollbackSpec::Define()
{
	Describe("Snapshot / Restore — existing file", [this]()
	{
		It("restores the original bytes when the fix modified the file", [this]()
		{
			const FString Path = MakeFixturePath(TEXT("existing"));
			const FString Original = TEXT("original-content-v1");
			TestTrue(TEXT("write fixture"), WriteFile(Path, Original));

			FUnrealOpenMcpFixRollback Rollback;
			Rollback.Snapshot({Path});
			TestTrue(TEXT("has snapshot"), Rollback.HasSnapshot());

			// Simulate the fix mutating the file.
			TestTrue(TEXT("overwrite"), WriteFile(Path, TEXT("modified-by-fix")));

			const FUnrealOpenMcpFixRollbackRestore Restore = Rollback.Restore();
			TestEqual(TEXT("one restored path"), Restore.RestoredPaths.Num(), 1);

			FString After;
			TestTrue(TEXT("read back"), ReadFile(Path, After));
			TestEqual(TEXT("content restored"), After, Original);

			DeleteFileIfExists(Path);
		});
	});

	Describe("Snapshot / Restore — non-existent file (create-case rollback)", [this]()
	{
		It("deletes the file the fix created when the snapshot saw nothing", [this]()
		{
			const FString Path = MakeFixturePath(TEXT("create"));
			DeleteFileIfExists(Path);
			TestFalse(TEXT("does not exist before"), FileExists(Path));

			FUnrealOpenMcpFixRollback Rollback;
			Rollback.Snapshot({Path});

			// Simulate the fix creating the file.
			TestTrue(TEXT("fix creates"), WriteFile(Path, TEXT("fix-created")));
			TestTrue(TEXT("exists after fix"), FileExists(Path));

			const FUnrealOpenMcpFixRollbackRestore Restore = Rollback.Restore();
			TestEqual(TEXT("zero restored"), Restore.RestoredPaths.Num(), 0);
			TestEqual(TEXT("one deleted"), Restore.DeletedPaths.Num(), 1);
			TestFalse(TEXT("file removed"), FileExists(Path));
		});
	});

	Describe("Lifecycle", [this]()
	{
		It("HasSnapshot is false before Snapshot is called", [this]()
		{
			FUnrealOpenMcpFixRollback Rollback;
			TestFalse(TEXT("no snapshot"), Rollback.HasSnapshot());
		});

		It("Discard releases the snapshot without writing anything", [this]()
		{
			const FString Path = MakeFixturePath(TEXT("discard"));
			TestTrue(TEXT("write fixture"), WriteFile(Path, TEXT("v1")));

			FUnrealOpenMcpFixRollback Rollback;
			Rollback.Snapshot({Path});
			TestTrue(TEXT("has snapshot"), Rollback.HasSnapshot());

			// Overwrite then discard — the file must stay modified (Discard is
			// a no-write release, used by the runner after a clean commit).
			TestTrue(TEXT("overwrite"), WriteFile(Path, TEXT("v2")));
			Rollback.Discard();
			TestFalse(TEXT("no snapshot after discard"), Rollback.HasSnapshot());

			FString After;
			TestTrue(TEXT("read"), ReadFile(Path, After));
			TestEqual(TEXT("stays v2"), After, FString(TEXT("v2")));

			DeleteFileIfExists(Path);
		});

		It("de-duplicates when the same path is snapshotted twice", [this]()
		{
			const FString Path = MakeFixturePath(TEXT("dedup"));
			TestTrue(TEXT("write fixture"), WriteFile(Path, TEXT("v1")));

			FUnrealOpenMcpFixRollback Rollback;
			Rollback.Snapshot({Path, Path}); // same path twice
			Rollback.Snapshot({Path});        // third pass

			// Modify then restore — exactly one restored path entry (de-dupe).
			TestTrue(TEXT("overwrite"), WriteFile(Path, TEXT("v2")));
			const FUnrealOpenMcpFixRollbackRestore Restore = Rollback.Restore();
			TestEqual(TEXT("one restored"), Restore.RestoredPaths.Num(), 1);

			DeleteFileIfExists(Path);
		});

		It("Restore is safe to call when no snapshot was taken", [this]()
		{
			FUnrealOpenMcpFixRollback Rollback;
			const FUnrealOpenMcpFixRollbackRestore Restore = Rollback.Restore();
			TestEqual(TEXT("no restored paths"), Restore.RestoredPaths.Num(), 0);
			TestEqual(TEXT("no deleted paths"), Restore.DeletedPaths.Num(), 0);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
