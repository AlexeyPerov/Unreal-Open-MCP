// FFixRollback implementation. See header for the snapshot/restore contract.
#include "Gate/UnrealOpenMcpFixRollback.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

void FUnrealOpenMcpFixRollback::Snapshot(const TArray<FString>& Paths)
{
	bHasSnapshot = true;
	for (const FString& Path : Paths)
	{
		if (Path.IsEmpty())
		{
			continue;
		}

		// De-dupe by absolute path. A caller that passes the same file twice
		// (e.g. when the issue asset path resolves to the same .uasset the
		// package-name form would) gets one snapshot entry.
		const FString Normalized = FPaths::ConvertRelativePathToFull(Path);
		bool bAlready = false;
		for (const FEntry& Existing : Entries)
		{
			if (Existing.Path == Normalized)
			{
				bAlready = true;
				break;
			}
		}
		if (bAlready)
		{
			continue;
		}

		FEntry Entry;
		Entry.Path = Normalized;
		if (FPaths::FileExists(Normalized))
		{
			// Read raw bytes — UE assets are binary (UDK-style .uasset). The
			// restore writes them back verbatim so a fix that grew the bulk
			// payload cannot leak extra bytes the original did not have.
			if (FFileHelper::LoadFileToArray(Entry.Bytes, *Normalized))
			{
				Entry.bExisted = true;
			}
			else
			{
				// File exists but could not be read (locked, permissions).
				// Record as did-not-exist so Restore() does not blindly delete
				// the file — the safer fallback is to leave it alone and let
				// the gate delta surface the failure mode.
				Entry.bExisted = false;
				Entry.Bytes.Reset();
			}
		}
		else
		{
			Entry.bExisted = false;
		}
		Entries.Add(MoveTemp(Entry));
	}
}

FUnrealOpenMcpFixRollbackRestore FUnrealOpenMcpFixRollback::Restore()
{
	FUnrealOpenMcpFixRollbackRestore Out;
	if (!bHasSnapshot)
	{
		return Out;
	}

	for (const FEntry& Entry : Entries)
	{
		if (Entry.bExisted)
		{
			// Write the captured bytes back. A failure here is logged but does
			// not abort the restore loop — partial restoration is strictly
			// better than no restoration, and the operator can triage via the
			// gate delta (the post-restore validate pass will still flag the
			// original issue + any new ones).
			if (FFileHelper::SaveArrayToFile(Entry.Bytes, *Entry.Path))
			{
				Out.RestoredPaths.Add(Entry.Path);
			}
		}
		else
		{
			// Did not exist pre-fix — delete the file the fix created.
			if (FPaths::FileExists(Entry.Path))
			{
				IFileManager::Get().Delete(*Entry.Path);
				Out.DeletedPaths.Add(Entry.Path);
			}
		}
	}
	return Out;
}

void FUnrealOpenMcpFixRollback::Discard()
{
	Entries.Reset();
	bHasSnapshot = false;
}
