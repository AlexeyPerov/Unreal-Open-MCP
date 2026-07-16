// Object-reference resolution for actor-tool handlers.
//
// The Unreal analog of Unity Open MCP's TypedTargets
// (packages/bridge/Editor/ObjectRefs/TypedTargets.cs): a single string
// identifies an actor, resolved with a label → name → path fallback chain.
// There is no global integer instance id in Unreal (unlike Unity), so the
// addressing surface is string-only. Behavior reference:
// Unreal-MCP's FUnrealMcpObjectRef (read-only) — same label/name/path
// precedence with a case-sensitive-exact-first + case-insensitive-fallback
// sweep, adapted to run inside the UnrealOpenMcpEditor module.
//
// Every method here runs ON THE GAME THREAD (it touches GEditor, the editor
// world, and the UObject graph). The HTTP server marshals every tool dispatch
// through the GameThreadDispatcher before a handler is invoked, so callers can
// use these helpers freely from inside a FUnrealOpenMcpToolHandler.
//
// Fidelity (P2.2):
//   - GetEditorWorld / ResolveActor / ResolveClass / ActorIdentity — adapt
//     from Unreal-MCP FUnrealMcpObjectRef (label → name → path, exact-first).
//   - Targeted-miss behavior — copy Unity's gameobject-find contract
//     (targeted miss is NOT an error; it returns {ok:true,notFound:true}).
// P2.4 adds ResolveObject (generic UObject by actor label, loaded-object path,
// or asset soft path) — the addressing surface object_modify needs.
// P2.5 adds ResolveComponent (component by name / readable-name / class on an
// actor) — the addressing surface the component CRUD tools need.
#pragma once

#include "CoreMinimal.h"

class AActor;
class UActorComponent;
class UClass;
class UObject;
class UWorld;

/**
 * Actor / class / object reference resolution shared by the actor-tool family.
 * Lives in a self-contained header so later actor tools (create / modify /
 * tree ops / object reflection) reuse the same resolver and keep addressing
 * semantics identical across the family. Read-only helpers — none of them
 * mutate the world.
 */
namespace FUnrealOpenMcpObjectRef
{
	/**
	 * The editor world the actor family operates on
	 * (GEditor->GetEditorWorldContext().World()). Returns null outside the
	 * editor / before the editor world is available. Mirrors Unity's
	 * "active scene" accessor scoped to the editor world only.
	 */
	UNREALOPENMCPEDITOR_API UWorld* GetEditorWorld();

	/**
	 * Resolve an actor reference within @p World (defaults to the editor world
	 * when null). Matches, in order, a case-SENSITIVE then case-INSENSITIVE
	 * sweep over the actor label, the UObject name, and the full path name.
	 * Returns the first exact match, falling back to the first
	 * case-insensitive label match. Null when nothing matches.
	 *
	 * The two-pass behavior (exact-first, then loose) mirrors Unreal-MCP's
	 * FUnrealMcpObjectRef::ResolveActor so a loosely-cased LLM-supplied label
	 * still resolves while a deterministic exact match is always preferred.
	 */
	UNREALOPENMCPEDITOR_API AActor* ResolveActor(const FString& ActorRef, UWorld* World = nullptr);

	/**
	 * Resolve a class reference to a UClass*. Accepts, in order:
	 *   - a soft class path (`/Script/Engine.PointLight`,
	 *     `/Game/BP/BP_Foo.BP_Foo_C`),
	 *   - a loadable object path that is itself a UClass or a UBlueprint
	 *     (returns its GeneratedClass),
	 *   - a short native type name (`StaticMeshActor`, `PointLight`).
	 * Returns null when nothing matches. Used by the list-mode `class` filter.
	 */
	UNREALOPENMCPEDITOR_API UClass* ResolveClass(const FString& ClassRef);

	/**
	 * Resolve any UObject by reference — the addressing surface
	 * `object_modify` (and later `object_get_data`) needs. Resolution order:
	 *   1. A live actor in @p World (label → name → path) — takes precedence
	 *      so `object-*` can target scene actors just like the actor tools.
	 *   2. An already-loaded UObject by path (FindFirstObject).
	 *   3. A soft object path on disk (FSoftObjectPath::TryLoad) — loads the
	 *      asset into memory if not already there.
	 *   4. Last-resort StaticLoadObject for legacy short forms that
	 *      FSoftObjectPath's strict parsing rejects.
	 * Returns null when nothing matches. @p World defaults to the editor world
	 * when null (the actor check needs a world to sweep).
	 *
	 * Mirrors Unreal-MCP's FUnrealMcpObjectRef::ResolveObject (read-only
	 * reference) — same precedence chain so a ref that resolves as an actor in
	 * `actor_modify` also resolves in `object_modify`.
	 */
	UNREALOPENMCPEDITOR_API UObject* ResolveObject(const FString& ObjectRef, UWorld* World = nullptr);

	/**
	 * Resolve a component on @p Actor by name / readable-name / class. The
	 * addressing surface the P2.5 component CRUD tools (component_get /
	 * component_modify / component_destroy) need. Matches, in order:
	 *   - The component's UObject name (GetName) — case-insensitive.
	 *   - The component's readable name (GetReadableName) — case-insensitive.
	 *   - A class ref resolved via ResolveClass: returns the FIRST component on
	 *     the actor whose class IsA the resolved class (matching Unity's
	 *     GetComponent(type) first-match semantics). Mirrors Unity's
	 *     "When a type allows multiples, only the first match is removed" rule.
	 * Returns null when @p Actor is null, @p ComponentRef is empty, or nothing
	 * on the actor matches.
	 *
	 * Adapted (read-only) from Unreal-MCP's FUnrealMcpObjectRef::ResolveComponent
	 * (the GetName / GetReadableName iteration); the class-ref branch is added
	 * here to cover Unity's class-name component targeting without a second
	 * helper.
	 */
	UNREALOPENMCPEDITOR_API UActorComponent* ResolveComponent(
		AActor* Actor,
		const FString& ComponentRef);
}
