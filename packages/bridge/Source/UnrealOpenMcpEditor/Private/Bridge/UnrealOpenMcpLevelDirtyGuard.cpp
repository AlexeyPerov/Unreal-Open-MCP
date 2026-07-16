// Level dirty guard — see header for the package-dirty contract and the
// game-thread invariant.
//
// Adapted from Unity Open MCP's SceneDirtyGuard
// (packages/bridge/Editor/Bridge/SceneDirtyGuard.cs — the editor's dirty-scene
// probe) and Unreal-MCP's DirtyMapPackageNames helper (read-only reference for
// UEditorLoadingAndSavingUtils::GetDirtyMapPackages). Unity's guard tracks
// UnityEditor.SceneManagement.EditorSceneManager scenes; this port probes
// Unreal's package dirty state instead, since a level's unsaved edits live on
// its outermost UPackage and the editor's save-prompt keys off the same set.
#include "Bridge/UnrealOpenMcpLevelDirtyGuard.h"

// FileHelpers.h owns UEditorLoadingAndSavingUtils (the UnrealEd surface for
// GetDirtyMapPackages / LoadMap / SaveMap / SaveCurrentLevel). Lives in the
// UnrealEd module (already a Private dependency of UnrealOpenMcpEditor).
#include "Editor/FileHelpers.h"

namespace FUnrealOpenMcpLevelDirtyGuard
{
	bool AreDirtyMapsPresent()
	{
		return DirtyMapPackageNames().Num() > 0;
	}

	TArray<FString> DirtyMapPackageNames()
	{
		// GetDirtyMapPackages returns the set of map packages the editor would
		// prompt to save — the same surface its save-prompt dialog keys off. A
		// transient/never-saved level reports its transient package here too, so
		// the guard correctly flags unsaved edits on a level with no on-disk
		// location yet.
		TArray<UPackage*> DirtyPackages;
		UEditorLoadingAndSavingUtils::GetDirtyMapPackages(DirtyPackages);

		TArray<FString> Names;
		Names.Reserve(DirtyPackages.Num());
		for (const UPackage* Package : DirtyPackages)
		{
			if (Package != nullptr)
			{
				Names.Add(Package->GetName());
			}
		}
		return Names;
	}
}
