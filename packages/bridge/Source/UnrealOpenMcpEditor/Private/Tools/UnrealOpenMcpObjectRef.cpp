// Object-reference resolution — see header for the label/name/path precedence
// and the game-thread contract.
//
// Behavior adapted from Unreal-MCP's FUnrealMcpObjectRef (read-only reference).
// That project routes GetEditorWorld through an injected world provider so the
// same code runs in a packaged game; this port is editor-only (the actor family
// lives in UnrealOpenMcpEditor), so GEditor is referenced directly. The
// resolve sweep (exact-first, then case-insensitive fallback) is preserved
// byte-for-byte because LLM-supplied labels are frequently loosely cased and
// the two-pass behavior is the load-bearing determinism contract.
#include "Tools/UnrealOpenMcpObjectRef.h"

#include "GameFramework/Actor.h"

#include "Engine/Blueprint.h"
#include "EngineUtils.h"            // TActorIterator
#include "Editor.h"                 // GEditor
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/SoftObjectPtr.h"

namespace FUnrealOpenMcpObjectRef
{
	UWorld* GetEditorWorld()
	{
		// GEditor is null outside the editor; the editor world context is the
		// single world the actor family operates on (loaded sublevels are
		// reachable through TActorIterator on this world). Mirrors Unity's
		// "active scene" surface scoped to the editor.
		if (GEditor == nullptr)
		{
			return nullptr;
		}
		return GEditor->GetEditorWorldContext().World();
	}

	AActor* ResolveActor(const FString& ActorRef, UWorld* World)
	{
		if (ActorRef.IsEmpty())
		{
			return nullptr;
		}
		if (World == nullptr)
		{
			World = GetEditorWorld();
		}
		if (World == nullptr)
		{
			return nullptr;
		}

		// FString::operator== is case-INSENSITIVE in UE, so an explicit
		// ESearchCase::CaseSensitive Equals is required to make the exact-first
		// preference real and keep the case-insensitive fallback reachable.
		// A single sweep: any exact match returns immediately; otherwise we
		// remember the FIRST case-insensitive label match (LLM-supplied labels
		// are often loosely cased) and return it after the sweep — same result
		// a second pass would produce, without walking the actor list twice.
		AActor* IgnoreCaseFallback = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor == nullptr)
			{
				continue;
			}
			// AActor::GetActorLabel() is the editor-visible friendly label; in
			// an editor module it is always available (no WITH_EDITOR guard
			// needed — this file only compiles for the editor target).
			if (Actor->GetActorLabel().Equals(ActorRef, ESearchCase::CaseSensitive)
				|| Actor->GetName().Equals(ActorRef, ESearchCase::CaseSensitive)
				|| Actor->GetPathName().Equals(ActorRef, ESearchCase::CaseSensitive))
			{
				return Actor;
			}
			if (IgnoreCaseFallback == nullptr
				&& Actor->GetActorLabel().Equals(ActorRef, ESearchCase::IgnoreCase))
			{
				IgnoreCaseFallback = Actor;
			}
		}

		return IgnoreCaseFallback;
	}

	UClass* ResolveClass(const FString& ClassRef)
	{
		if (ClassRef.IsEmpty())
		{
			return nullptr;
		}

		// 1. Soft class path — handles native (`/Script/Engine.PointLight`) and
		//    generated Blueprint classes (`/Game/BP/BP_Foo.BP_Foo_C`) without
		//    forcing a synchronous asset scan first.
		if (UClass* Loaded = FSoftClassPath(ClassRef).TryLoadClass<UObject>())
		{
			return Loaded;
		}

		// 2. Load as a generic object: the ref may point at a UClass directly,
		//    or at a UBlueprint asset whose GeneratedClass is what callers want.
		//    LOAD_NoWarn | LOAD_Quiet: short unqualified names (`PointLight`)
		//    reach here and fail this load before succeeding at step 3 — without
		//    the flags each emits a spurious LogUObjectGlobals warning.
		if (UObject* Loaded = StaticLoadObject(
				UObject::StaticClass(), nullptr, *ClassRef, nullptr,
				LOAD_NoWarn | LOAD_Quiet))
		{
			if (UClass* AsClass = Cast<UClass>(Loaded))
			{
				return AsClass;
			}
			if (const UBlueprint* AsBlueprint = Cast<UBlueprint>(Loaded))
			{
				return AsBlueprint->GeneratedClass;
			}
		}

		// 3. Short, unqualified native type name (`StaticMeshActor`,
		//    `PointLight`). TryFindTypeSlow covers the native transients;
		//    FindFirstObject is the last-resort lookup for anything already in
		//    memory under that name.
		if (UClass* Found = UClass::TryFindTypeSlow<UClass>(ClassRef))
		{
			return Found;
		}

		return FindFirstObject<UClass>(*ClassRef, EFindFirstObjectOptions::None);
	}

	UObject* ResolveObject(const FString& ObjectRef, UWorld* World)
	{
		if (ObjectRef.IsEmpty())
		{
			return nullptr;
		}

		// 1. A live actor (by label/name/path) takes precedence so `object_*`
		//    can target scene actors just like the actor tools. ResolveActor
		//    sweeps the editor world, so a ref that resolves in actor_modify
		//    also resolves here.
		if (AActor* Actor = ResolveActor(ObjectRef, World))
		{
			return Actor;
		}

		// 2. Already-loaded object by path. FindFirstObject is the UE5
		//    non-deprecated lookup (FindObject is deprecated); EFindFirstObject
		//    options = None matches ResolveClass's last-resort lookup.
		if (UObject* Found = FindFirstObject<UObject>(*ObjectRef, EFindFirstObjectOptions::None))
		{
			return Found;
		}

		// 3. Soft path load — an asset on disk not yet in memory. TryLoad
		//    returns a UObject* (any class); null when the path is malformed
		//    or points at nothing.
		if (UObject* Loaded = FSoftObjectPath(ObjectRef).TryLoad())
		{
			return Loaded;
		}

		// 4. Last-resort fallback for refs that FSoftObjectPath's strict
		//    parsing rejects (e.g. legacy `Package.Object` short forms).
		//    Overlaps TryLoad for well-formed paths; kept for that long tail.
		//    LOAD_NoWarn | LOAD_Quiet (matching ResolveClass): every
		//    object-modify miss funnels through here, so without the flags a
		//    legitimate "not found" emits a spurious LogUObjectGlobals warning.
		return StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectRef, nullptr,
			LOAD_NoWarn | LOAD_Quiet);
	}
}
