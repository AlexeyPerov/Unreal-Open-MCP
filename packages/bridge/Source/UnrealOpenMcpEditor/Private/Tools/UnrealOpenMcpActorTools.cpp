// Actor-tool family — see header for the find/resolve contract and the
// targeted-miss-vs-error semantics. This file owns the `actor_find`,
// `actor_create`, `actor_modify`, and `object_modify` handlers plus the shared
// actor-data serializer (`ToActorData`) that later actor tools (tree ops) will
// reuse for consistent output.
//
// Arg parsing: each handler receives the raw POST body FString (the registry
// contract is raw-body; each tool owns its arg extraction). The handlers parse
// the body into an FJsonObject via the Json module (TJsonReader +
// FJsonSerializer). The registry/dispatch spine stays raw-body — only the
// handler layer parses.
//
// Output: built with FJsonObject + JsonSerializer into a pre-serialized string
// handed to FUnrealOpenMcpToolDispatchResult::Output. (The pinned /ping field
// order is a BridgeJson-only contract; tool payloads may use the JsonObject
// writer freely.)
#include "Tools/UnrealOpenMcpActorTools.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Tools/UnrealOpenMcpObjectRef.h"
// P2.4 — shared FProperty write helper used by actor_modify + object_modify.
#include "Tools/UnrealOpenMcpPropertyJson.h"
#include "UnrealOpenMcpLog.h"

#include "GameFramework/Actor.h"
#include "EngineUtils.h"                // TActorIterator
#include "Components/ActorComponent.h"
// P2.3 — FAttachmentTransformRules for AttachToActor (KeepWorldTransform keeps
// the spawn pose stable across the reparent, matching the behavior reference).
// P2.5 reuses them for actor_set_parent (KeepWorld vs KeepRelative on request).
#include "Components/SceneComponent.h"
// P2.3 — actor_create wraps the spawn in FScopedTransaction so the new actor
// is undoable from the editor (Ctrl+Z). Mirrors Unity's
// Undo.RegisterCreatedObjectUndo at the transaction granularity. P2.4 reuses
// it for actor_modify / object_modify so property writes group as one undo.
// P2.5 reuses it for the tree ops + component mutators.
#include "ScopedTransaction.h"
// P2.3 — SetActorLabelUnique avoids label collisions that would make two
// actors ambiguous to ResolveActor (label → name → path). Lives in UnrealEd.
// P2.5 reuses it for actor_duplicate.
#include "ActorEditorUtils.h"
// P2.5 — StaticFindObjectFast (UObjectGlobals) + MakeUniqueObjectName guard
// component_add against a name collision that would otherwise trip a
// StaticAllocateObject fatal assert inside NewObject (behavior reference:
// UnrealMcpActorTools.cpp).
#include "UObject/UObjectGlobals.h"
#include "UObject/Object.h"             // UObject base for StaticFindObjectFast

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
// P2.5 — component_get reflects the component's Edit/Blueprint-visible
// FProperties into a JSON object via UStructToJsonObject (the read counterpart
// of the JsonValueToUProperty writes the mutate helper performs).
#include "JsonObjectConverter.h"

namespace
{
	/** Default list-mode result cap. Mirrors Unity's gameobject-find default. */
	constexpr int32 DefaultMaxResults = 25;
	/** Hard ceiling on list-mode results so a huge level never floods the
	 *  response. Mirrors the P2.2 plan's "hard cap 100". */
	constexpr int32 MaxResultsCap = 100;

	/**
	 * Parse the raw POST body into a JSON object. Returns null when the body is
	 * empty or not a JSON object (the caller surfaces a structured
	 * `invalid_parameter` error). Used by every actor tool that takes structured
	 * args — kept here so the parsing contract is identical across the family.
	 */
	TSharedPtr<FJsonObject> ParseBody(const FString& Body)
	{
		const FString Trimmed = Body.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			// An empty body is a valid "no args" call — treat as an empty object
			// so handlers can resolve optional fields with their defaults.
			return MakeShared<FJsonObject>();
		}
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			return nullptr;
		}
		return Object;
	}

	/** Clamp the max_results argument into [1, MaxResultsCap]. Invalid (non-int,
	 *  negative) values fall back to the default. */
	int32 ResolveMaxResults(const TSharedPtr<FJsonObject>& Args)
	{
		if (!Args.IsValid() || !Args->HasField(TEXT("max_results")))
		{
			return DefaultMaxResults;
		}
		// HasTypedField distinguishes a real number from a string/bool/null; a
		// non-number field is treated as "use the default" rather than an error
		// so a loosely-typed caller still gets a bounded result.
		if (!Args->HasTypedField<EJson::Number>(TEXT("max_results")))
		{
			return DefaultMaxResults;
		}
		const int32 Requested = static_cast<int32>(Args->GetNumberField(TEXT("max_results")));
		if (Requested < 1)
		{
			return DefaultMaxResults;
		}
		return FMath::Min(Requested, MaxResultsCap);
	}

	/**
	 * Read a `{x,y,z}` object field as an FVector. Returns the default when the
	 * field is absent or not a JSON object. Used by actor_create for location +
	 * scale. Loose-typed on purpose (a missing axis falls back to the default
	 * component) so a caller that sends only `{x,y}` still gets a sane vector.
	 */
	FVector ReadVectorField(const TSharedPtr<FJsonObject>& Args, const FString& FieldName, const FVector& Default)
	{
		if (!Args.IsValid() || !Args->HasTypedField<EJson::Object>(FieldName))
		{
			return Default;
		}
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		Args->TryGetObjectField(FieldName, ObjPtr);
		if (ObjPtr == nullptr || !ObjPtr->IsValid())
		{
			return Default;
		}
		const TSharedPtr<FJsonObject>& Obj = *ObjPtr;
		FVector Out = Default;
		if (Obj->HasTypedField<EJson::Number>(TEXT("x")))
		{
			Out.X = Obj->GetNumberField(TEXT("x"));
		}
		if (Obj->HasTypedField<EJson::Number>(TEXT("y")))
		{
			Out.Y = Obj->GetNumberField(TEXT("y"));
		}
		if (Obj->HasTypedField<EJson::Number>(TEXT("z")))
		{
			Out.Z = Obj->GetNumberField(TEXT("z"));
		}
		return Out;
	}

	/**
	 * Read a `{pitch,yaw,roll}` (degrees) object field as an FRotator. Returns
	 * the default when absent or not an object. Used by actor_create for the
	 * world rotation argument.
	 */
	FRotator ReadRotatorField(const TSharedPtr<FJsonObject>& Args, const FString& FieldName, const FRotator& Default)
	{
		if (!Args.IsValid() || !Args->HasTypedField<EJson::Object>(FieldName))
		{
			return Default;
		}
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		Args->TryGetObjectField(FieldName, ObjPtr);
		if (ObjPtr == nullptr || !ObjPtr->IsValid())
		{
			return Default;
		}
		const TSharedPtr<FJsonObject>& Obj = *ObjPtr;
		FRotator Out = Default;
		if (Obj->HasTypedField<EJson::Number>(TEXT("pitch")))
		{
			Out.Pitch = Obj->GetNumberField(TEXT("pitch"));
		}
		if (Obj->HasTypedField<EJson::Number>(TEXT("yaw")))
		{
			Out.Yaw = Obj->GetNumberField(TEXT("yaw"));
		}
		if (Obj->HasTypedField<EJson::Number>(TEXT("roll")))
		{
			Out.Roll = Obj->GetNumberField(TEXT("roll"));
		}
		return Out;
	}

	/** Serialize a rotator (pitch/yaw/roll, degrees) into a JsonObject. */
	TSharedRef<FJsonObject> RotatorToJson(const FRotator& R)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("pitch"), R.Pitch);
		Json->SetNumberField(TEXT("yaw"), R.Yaw);
		Json->SetNumberField(TEXT("roll"), R.Roll);
		return Json;
	}

	/** Serialize a vector into a JsonObject. */
	TSharedRef<FJsonObject> VectorToJson(const FVector& V)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("x"), V.X);
		Json->SetNumberField(TEXT("y"), V.Y);
		Json->SetNumberField(TEXT("z"), V.Z);
		return Json;
	}

	/** Serialize a transform into a JsonObject: location, rotation, scale. */
	TSharedRef<FJsonObject> TransformToJson(const FTransform& Transform)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetObjectField(TEXT("location"), VectorToJson(Transform.GetLocation()));
		Json->SetObjectField(TEXT("rotation"), RotatorToJson(Transform.Rotator()));
		Json->SetObjectField(TEXT("scale"), VectorToJson(Transform.GetScale3D()));
		return Json;
	}

	/**
	 * Serialize one actor into the `ActorData` payload documented in the P2.2
	 * contract: { label, name, class, path, location, rotation, scale, components? }.
	 * Identity fields are always present; the transform is the actor's
	 * world-space transform; components are emitted as a short-name array and
	 * may be omitted via @p bIncludeComponents=false to save tokens.
	 *
	 * Mirrors Unity's BuildGameObjectSummary (the field set an agent chains from
	 * into modify/set_parent), adapted to Unreal's actor surface (label instead
	 * of instance_id; pitch/yaw/roll instead of euler x/y/z).
	 */
	TSharedRef<FJsonObject> ToActorData(AActor* Actor, bool bIncludeComponents)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();

		Json->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Json->SetStringField(TEXT("name"), Actor->GetName());
		Json->SetStringField(
			TEXT("class"),
			Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
		Json->SetStringField(TEXT("path"), Actor->GetPathName());
		Json->SetObjectField(TEXT("transform"), TransformToJson(Actor->GetTransform()));

		if (bIncludeComponents)
		{
			TArray<TSharedPtr<FJsonValue>> Components;
			for (UActorComponent* Component : Actor->GetComponents())
			{
				if (Component == nullptr)
				{
					continue;
				}
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Component->GetName());
				Entry->SetStringField(
					TEXT("class"),
					Component->GetClass() ? Component->GetClass()->GetPathName() : FString());
				Components.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Json->SetArrayField(TEXT("components"), Components);
		}

		return Json;
	}

	/** Serialize a JsonObject to a compact string. Returns "null" on a null
	 *  pointer so the result is always valid JSON. */
	FString WriteJson(const TSharedPtr<FJsonValue>& JsonValue)
	{
		if (!JsonValue.IsValid())
		{
			return TEXT("null");
		}
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		if (FJsonSerializer::Serialize(JsonValue, Writer))
		{
			return Out;
		}
		return TEXT("null");
	}

	/**
	 * Serialize one component into the `ComponentData` payload documented in the
	 * P2.5 contract: { name, class, properties? }. The properties object is the
	 * UScriptStruct→JSON reflection of the component's Edit/Blueprint-visible
	 * FProperties (FJsonObjectConverter::UStructToJsonObject), so a component_get
	 * result carries the values an agent would write back via component_modify.
	 * Omitted when @p bIncludeProperties is false (saves tokens for list_all).
	 *
	 * Mirrors Unity's component-get field dump (ComponentsTools.Get), adapted to
	 * Unreal's FProperty reflection (UStructToJsonObject instead of
	 * SerializedObject iteration).
	 */
	TSharedRef<FJsonObject> ToComponentData(UActorComponent* Component, bool bIncludeProperties)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Component->GetName());
		Json->SetStringField(
			TEXT("class"),
			Component->GetClass() ? Component->GetClass()->GetPathName() : FString());

		if (bIncludeProperties)
		{
			// UStructToJsonObject reflects the component's Edit/Blueprint-visible
			// FProperties into a JSON object (the read counterpart of the
			// FJsonObjectConverter::JsonValueToUProperty writes the mutate helper
			// performs). CheckFlags=0 + SkipFlags=0 mirrors ApplyProperties'
			// permissiveness so a get+modify round-trip is symmetric.
			TSharedPtr<FJsonObject> Props;
			if (FJsonObjectConverter::UStructToJsonObject(
					Component->GetClass(), Component, Props,
					/*CheckFlags*/ 0, /*SkipFlags*/ 0))
			{
				Json->SetObjectField(TEXT("properties"), Props);
			}
		}
		return Json;
	}

	/**
	 * Resolve a component class ref and validate it for the component_add /
	 * component_destroy surface. Returns the UClass on success; on failure sets
	 * @p OutCode + @p OutMessage to a structured error and returns null.
	 * Validation mirrors actor_create's class gate: must resolve, must be a
	 * UActorComponent subclass, must not be abstract (a CreateComponent on an
	 * abstract class would assert or return null at runtime).
	 */
	UClass* ResolveComponentClass(const FString& ClassRef, FString& OutCode, FString& OutMessage)
	{
		if (ClassRef.IsEmpty())
		{
			OutCode = TEXT("missing_parameter");
			OutMessage = TEXT("'componentClass' is required and must be a non-empty string.");
			return nullptr;
		}
		UClass* Class = FUnrealOpenMcpObjectRef::ResolveClass(ClassRef);
		if (Class == nullptr)
		{
			OutCode = TEXT("class_not_found");
			OutMessage = FString::Printf(
				TEXT("componentClass '%s' did not resolve to a class."),
				*ClassRef);
			return nullptr;
		}
		if (!Class->IsChildOf(UActorComponent::StaticClass()))
		{
			OutCode = TEXT("invalid_parameter");
			OutMessage = FString::Printf(
				TEXT("'%s' resolved but is not a UActorComponent class."),
				*ClassRef);
			return nullptr;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract))
		{
			OutCode = TEXT("invalid_parameter");
			OutMessage = FString::Printf(
				TEXT("'%s' is abstract and cannot be instantiated."),
				*ClassRef);
			return nullptr;
		}
		return Class;
	}
}

void FUnrealOpenMcpActorTools::Register(FUnrealOpenMcpToolRegistry& Registry)
{
	// unreal_open_mcp_actor_find — read-only actor locator. Two modes:
	//   (a) targeted: `actor` ref string (label → name → path) → single actor.
	//       A targeted miss is NOT an error; it returns {ok:true, notFound:true}
	//       with an empty actors[] so an agent can branch on "no match" without
	//       parsing an error. (Copied from Unity's gameobject-find contract.)
	//   (b) list: omit `actor` → enumerate the editor world with optional
	//       `class` / `name_contains` filters, bounded by max_results (default
	//       25, hard cap 100). `truncated` reports the count clipped by the cap.
	// Structured errors: `no_editor_world` (no GEditor/world), `invalid_parameter`
	// (malformed body or bad type).
	Registry.Register(
		TEXT("unreal_open_mcp_actor_find"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			// --- Targeted mode: a non-empty `actor` ref resolves to ONE actor. ---
			const FString ActorRef = Args->HasTypedField<EJson::String>(TEXT("actor"))
				? Args->GetStringField(TEXT("actor"))
				: FString();
			if (!ActorRef.IsEmpty())
			{
				AActor* Resolved = FUnrealOpenMcpObjectRef::ResolveActor(ActorRef, World);
				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> Actors;
				if (Resolved != nullptr)
				{
					Actors.Add(MakeShared<FJsonValueObject>(ToActorData(Resolved, /*bIncludeComponents=*/true)));
					Result->SetBoolField(TEXT("notFound"), false);
				}
				else
				{
					// Targeted miss — {ok:true, notFound:true, actors:[]}. NOT an
					// error so an agent can distinguish "no match" from a bad
					// request. Mirrors Unity's gameobject-find targeted path.
					Result->SetBoolField(TEXT("notFound"), true);
				}
				Result->SetArrayField(TEXT("actors"), Actors);
				Result->SetNumberField(TEXT("count"), Actors.Num());
				return FUnrealOpenMcpToolDispatchResult::Ok(
					WriteJson(MakeShared<FJsonValueObject>(Result)));
			}

			// --- List mode: filters + bounded enumeration. ---
			const FString ClassFilter = Args->HasTypedField<EJson::String>(TEXT("class"))
				? Args->GetStringField(TEXT("class"))
				: FString();
			const FString NameContains = Args->HasTypedField<EJson::String>(TEXT("name_contains"))
				? Args->GetStringField(TEXT("name_contains"))
				: FString();

			// Resolve the class filter up front so a bad class ref is an early,
			// structured error rather than a silent empty result.
			UClass* FilterClass = nullptr;
			if (!ClassFilter.IsEmpty())
			{
				FilterClass = FUnrealOpenMcpObjectRef::ResolveClass(ClassFilter);
				if (FilterClass == nullptr)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						FString::Printf(
							TEXT("class '%s' did not resolve to a class."),
							*ClassFilter));
				}
			}

			const int32 MaxResults = ResolveMaxResults(Args);

			TArray<TSharedPtr<FJsonValue>> Actors;
			int32 TotalMatched = 0;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (Actor == nullptr)
				{
					continue;
				}
				// Filter: class (IsA covers subclasses, matching Unity's component
				// filter intent) and name_contains (case-insensitive label match).
				if (FilterClass != nullptr && !Actor->IsA(FilterClass))
				{
					continue;
				}
				if (!NameContains.IsEmpty()
					&& !Actor->GetActorLabel().Contains(NameContains, ESearchCase::IgnoreCase))
				{
					continue;
				}

				++TotalMatched;
				// Keep counting total matches so `truncated` is accurate even
				// after the cap is reached; stop materializing entries past it.
				if (Actors.Num() >= MaxResults)
				{
					continue;
				}
				Actors.Add(MakeShared<FJsonValueObject>(
					ToActorData(Actor, /*bIncludeComponents=*/false)));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("actors"), Actors);
			Result->SetNumberField(TEXT("count"), Actors.Num());
			Result->SetNumberField(TEXT("truncated"), FMath::Max(0, TotalMatched - Actors.Num()));
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	// unreal_open_mcp_actor_create — first mutating actor tool. Spawns an actor
	// in the current editor level from a native class path
	// (e.g. `/Script/Engine.PointLight`, `StaticMeshActor`) or a Blueprint
	// asset path (`/Game/BP/BP_Foo.BP_Foo_C`), optionally setting label,
	// location, rotation, and a parent actor to attach to. Returns the new
	// actor's ActorData so the agent can chain actor_find / future modify /
	// tree tools without a second call.
	//
	// Wrapped in FScopedTransaction for editor Undo (mirrors Unity's
	// Undo.RegisterCreatedObjectUndo). `paths_hint` + `gate` are accepted on
	// the schema for forward-compat but NOT enforced until P3.5 — this is the
	// documented P2.3 deferral. Structured errors:
	//   - missing_parameter — classPath absent/empty
	//   - no_editor_world   — no GEditor / editor world
	//   - class_not_found   — classPath did not resolve to a UClass
	//   - invalid_parameter — resolved class is not an Actor / is abstract
	//   - parent_not_found  — parent ref did not resolve (nothing spawned)
	//   - spawn_failed      — SpawnActor returned null
	Registry.Register(
		TEXT("unreal_open_mcp_actor_create"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			// classPath is the one required arg. An empty/missing string is a
			// structured missing_parameter, not a class_not_found, so an agent
			// can distinguish "I forgot the arg" from "the path was wrong".
			const FString ClassPath = Args->HasTypedField<EJson::String>(TEXT("classPath"))
				? Args->GetStringField(TEXT("classPath"))
				: FString();
			if (ClassPath.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'classPath' is required and must be a non-empty string."));
			}

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			// Resolve the class. ResolveClass accepts native soft paths, BP
			// generated-class paths, BP asset paths, and short type names.
			UClass* Class = FUnrealOpenMcpObjectRef::ResolveClass(ClassPath);
			if (Class == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("class_not_found"),
					FString::Printf(
						TEXT("classPath '%s' did not resolve to a class."),
						*ClassPath));
			}
			// Guard the spawn: only Actor subclasses can be spawned, and
			// abstract classes (the common ones like AActor / AGameMode base)
			// must be rejected up front or SpawnActor silently returns null.
			if (!Class->IsChildOf(AActor::StaticClass()))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					FString::Printf(
						TEXT("'%s' resolved but is not an Actor class."),
						*ClassPath));
			}
			if (Class->HasAnyClassFlags(CLASS_Abstract))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					FString::Printf(
						TEXT("'%s' is abstract and cannot be spawned."),
						*ClassPath));
			}

			// Resolve the parent BEFORE spawning: resolving after the spawn
			// would leak an orphan actor on a bad ref, and the error would
			// carry no identity for the leaked actor. (Behavior reference:
			// UnrealMcpActorTools.cpp — same ordering, same rationale.)
			const FString ParentRef = Args->HasTypedField<EJson::String>(TEXT("parent"))
				? Args->GetStringField(TEXT("parent"))
				: FString();
			AActor* Parent = nullptr;
			if (!ParentRef.IsEmpty())
			{
				Parent = FUnrealOpenMcpObjectRef::ResolveActor(ParentRef, World);
				if (Parent == nullptr)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("parent_not_found"),
						FString::Printf(
							TEXT("Parent actor '%s' was not found; nothing was spawned."),
							*ParentRef));
				}
			}

			const FString Label = Args->HasTypedField<EJson::String>(TEXT("name"))
				? Args->GetStringField(TEXT("name"))
				: FString();
			const FVector Location = ReadVectorField(Args, TEXT("location"), FVector::ZeroVector);
			const FRotator Rotation = ReadRotatorField(Args, TEXT("rotation"), FRotator::ZeroRotator);

			// FScopedTransaction opens the undo bracket; SpawnActor + the
			// label/attach Modify() calls record into it, and destruction
			// closes the bracket. RF_Transactional on the spawn flags ensures
			// the new actor itself is undo-trackable (matches the behavior
			// reference's SpawnParameters.ObjectFlags).
			const FScopedTransaction Transaction(
				NSLOCTEXT("UnrealOpenMcp", "ActorCreate", "MCP: Create Actor"));
			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transactional;
			AActor* Actor = World->SpawnActor<AActor>(Class, Location, Rotation, SpawnParams);
			if (Actor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("spawn_failed"),
					FString::Printf(
						TEXT("SpawnActor returned null for classPath '%s'."),
						*ClassPath));
			}

			// SetActorLabelUnique (not SetActorLabel): a user-supplied label
			// that collides with an existing actor's label would otherwise make
			// both ambiguous to ResolveActor. Empty label → keep engine default.
			if (!Label.IsEmpty())
			{
				FActorLabelUtilities::SetActorLabelUnique(Actor, Label);
			}

			// AttachToActor returns void and silently no-ops when the freshly
			// spawned actor has no root component (e.g. a rootless class), so a
			// `parent` request would otherwise be dropped while we report plain
			// success. Verify it took; since the spawn itself succeeded, surface
			// a warning in the result rather than failing and leaking the actor.
			FString AttachWarning;
			if (Parent != nullptr)
			{
				Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
				if (Actor->GetAttachParentActor() != Parent)
				{
					AttachWarning = FString::Printf(
						TEXT(" (warning: could not attach to parent '%s' — the spawned actor has no root component)"),
						*Parent->GetActorLabel());
				}
			}

			// Mark the level package dirty so the editor knows to save. The
			// outer of a spawned actor is its level (ULevel), whose outer is the
			// UWorld package; MarkPackageDirty on the level propagates the dirty
			// bit the editor's save prompt keys off.
			if (ULevel* Level = Actor->GetLevel())
			{
				Level->MarkPackageDirty();
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("actor"), ToActorData(Actor, /*bIncludeComponents=*/true));
			if (!AttachWarning.IsEmpty())
			{
				Result->SetStringField(TEXT("warning"), AttachWarning);
			}
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// unreal_open_mcp_actor_modify — FProperty writes on resolved actor(s).
	// Two targeting shapes:
	//   (a) single: `actor` ref string (label → name → path).
	//   (b) batch:  `actors` array of refs → apply the same patches to each.
	// Each actor is resolved BEFORE the transaction opens so a bad ref returns
	// actor_not_found with nothing mutated (resolve-before-mutate, the same
	// leak-prevention invariant as actor_create's parent resolution). The
	// `properties` object is a flat name→value bag; per-field errors accumulate
	// in `errors[]` and do NOT abort the batch (partial success is the norm).
	// Transform shortcuts (location/rotation/scale) live INSIDE `properties`
	// and are routed to the actor transform APIs by the shared helper — they
	// are not top-level args (matches the behavior reference's single bag).
	//
	// Wrapped in FScopedTransaction for editor Undo. `paths_hint` + `gate` are
	// accepted on the schema for forward-compat but NOT enforced until P3.5
	// (documented P2.4 deferral).
	// Structured errors:
	//   - missing_parameter — neither `actor` nor `actors` supplied
	//   - no_editor_world   — no GEditor / editor world
	//   - actor_not_found   — a ref did not resolve (nothing mutated)
	Registry.Register(
		TEXT("unreal_open_mcp_actor_modify"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			// Collect target refs. `actor` (single) and `actors` (array) are
			// mutually informative: a single `actor` is the common case; an
			// `actors` array fans the same patches out to each. An empty set is
			// a structured missing_parameter so an agent can tell it forgot the
			// target from "the ref didn't resolve".
			TArray<FString> Refs;
			if (Args->HasTypedField<EJson::String>(TEXT("actor")))
			{
				Refs.Add(Args->GetStringField(TEXT("actor")));
			}
			if (Args->HasTypedField<EJson::Array>(TEXT("actors")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
				Args->TryGetArrayField(TEXT("actors"), Arr);
				if (Arr != nullptr)
				{
					for (const TSharedPtr<FJsonValue>& V : *Arr)
					{
						if (V.IsValid() && V->Type == EJson::String)
						{
							const FString S = V->AsString();
							if (!S.IsEmpty())
							{
								Refs.Add(S);
							}
						}
					}
				}
			}
			if (Refs.Num() == 0)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("Either 'actor' (string) or 'actors' (array of strings) is required."));
			}

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			// Resolve every target BEFORE opening the transaction. A miss returns
			// actor_not_found with nothing mutated — opening the transaction first
			// would push an empty/partial entry onto the undo stack.
			TArray<AActor*> Targets;
			Targets.Reserve(Refs.Num());
			for (const FString& Ref : Refs)
			{
				AActor* Resolved = FUnrealOpenMcpObjectRef::ResolveActor(Ref, World);
				if (Resolved == nullptr)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("actor_not_found"),
						FString::Printf(
							TEXT("Actor '%s' was not found; nothing was modified."),
							*Ref));
				}
				Targets.Add(Resolved);
			}

			// The properties bag is required — without it the call is a no-op
			// and almost certainly a caller mistake. An empty object {} is
			// valid (applies nothing, returns applied:0) so an agent can probe
			// resolution; only a missing/non-object field is an error.
			if (!Args->HasTypedField<EJson::Object>(TEXT("properties")))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'properties' object is required (use an empty object {} to probe resolution)."));
			}
			const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
			Args->TryGetObjectField(TEXT("properties"), PropsPtr);
			if (PropsPtr == nullptr || !PropsPtr->IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("'properties' must be a JSON object of name -> value pairs."));
			}
			const TSharedPtr<FJsonObject>& Properties = *PropsPtr;

			const FScopedTransaction Transaction(
				NSLOCTEXT("UnrealOpenMcp", "ActorModify", "MCP: Modify Actor"));

			int32 TotalApplied = 0;
			TArray<FString> Errors;
			TArray<TSharedPtr<FJsonValue>> ActorResults;
			for (AActor* Actor : Targets)
			{
				TArray<FString> PerActorErrors;
				const int32 Applied = FUnrealOpenMcpPropertyJson::ApplyProperties(Actor, Properties, PerActorErrors);
				TotalApplied += Applied;
				Errors.Append(PerActorErrors);

				// Per-actor entry so a batch result stays attributable: the
				// agent can see which actor got which applied count and errors.
				// A single-actor call still emits the array (length 1) so the
				// shape is uniform across single/batch.
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
				Entry->SetStringField(TEXT("path"), Actor->GetPathName());
				Entry->SetNumberField(TEXT("applied"), Applied);
				Entry->SetObjectField(TEXT("actor"), ToActorData(Actor, /*bIncludeComponents=*/false));
				ActorResults.Add(MakeShared<FJsonValueObject>(Entry));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("applied"), TotalApplied);
			Result->SetArrayField(TEXT("actors"), ActorResults);
			if (Errors.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> ErrorValues;
				ErrorValues.Reserve(Errors.Num());
				for (const FString& E : Errors)
				{
					ErrorValues.Add(MakeShared<FJsonValueString>(E));
				}
				Result->SetArrayField(TEXT("errors"), ErrorValues);
			}
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// unreal_open_mcp_object_modify — FProperty writes on any resolved UObject
	// (actor, component, asset instance). Uses ResolveObject (actor → loaded →
	// soft-path → StaticLoadObject) so the same ref surface as actor_modify
	// works for components and asset instances too. Same flat `properties` bag,
	// same per-field error accumulation, same FScopedTransaction Undo bracket.
	//
	// `paths_hint` + `gate` are forward-compat (no-op until P3.5).
	// Structured errors:
	//   - missing_parameter — `object` absent/empty, or `properties` absent
	//   - no_editor_world   — no GEditor / editor world (ResolveObject needs it
	//                         for the actor sweep)
	//   - object_not_found  — ref did not resolve (nothing mutated)
	Registry.Register(
		TEXT("unreal_open_mcp_object_modify"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString ObjectRef = Args->HasTypedField<EJson::String>(TEXT("object"))
				? Args->GetStringField(TEXT("object"))
				: FString();
			if (ObjectRef.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'object' is required (actor label/name/path or object/asset path)."));
			}

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			UObject* Object = FUnrealOpenMcpObjectRef::ResolveObject(ObjectRef, World);
			if (Object == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("object_not_found"),
					FString::Printf(
						TEXT("Object '%s' was not found; nothing was modified."),
						*ObjectRef));
			}

			if (!Args->HasTypedField<EJson::Object>(TEXT("properties")))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'properties' object is required."));
			}
			const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
			Args->TryGetObjectField(TEXT("properties"), PropsPtr);
			if (PropsPtr == nullptr || !PropsPtr->IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("'properties' must be a JSON object of name -> value pairs."));
			}
			const TSharedPtr<FJsonObject>& Properties = *PropsPtr;

			const FScopedTransaction Transaction(
				NSLOCTEXT("UnrealOpenMcp", "ObjectModify", "MCP: Modify Object"));

			TArray<FString> Errors;
			const int32 Applied = FUnrealOpenMcpPropertyJson::ApplyProperties(Object, Properties, Errors);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("applied"), Applied);
			Result->SetStringField(TEXT("name"), Object->GetName());
			Result->SetStringField(TEXT("class"), Object->GetClass() ? Object->GetClass()->GetPathName() : FString());
			Result->SetStringField(TEXT("path"), Object->GetPathName());
			if (Errors.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> ErrorValues;
				ErrorValues.Reserve(Errors.Num());
				for (const FString& E : Errors)
				{
					ErrorValues.Add(MakeShared<FJsonValueString>(E));
				}
				Result->SetArrayField(TEXT("errors"), ErrorValues);
			}
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// =====================================================================
	// P2.5 — actor tree ops + component CRUD tools.
	// =====================================================================

	// unreal_open_mcp_actor_set_parent — reparent an actor under another actor
	// in the current editor level. AttachToActor attaches the child's root
	// component to the parent actor; KeepWorldTransform (default) preserves the
	// world transform across the reparent (matches Unity's
	// world_position_stays=true). Cycle-safe: self-parent is rejected, and an
	// attach that would form a cycle (parent is already a descendant of child)
	// is rejected up front via IsAttachedTo before the attach runs (the engine
	// would otherwise silently drop the attach). Resolve-before-attach ordering:
	// both actors resolve BEFORE the transaction opens so a miss returns
	// actor_not_found / parent_not_found with nothing mutated.
	//
	// Wrapped in FScopedTransaction for editor Undo. `paths_hint` + `gate` are
	// accepted on the schema for forward-compat but NOT enforced until P3.5
	// (documented P2.5 deferral). Structured errors:
	//   - missing_parameter — neither `actor` nor `parent` supplied
	//   - no_editor_world   — no GEditor / editor world
	//   - actor_not_found   — child ref did not resolve (nothing mutated)
	//   - parent_not_found  — parent ref did not resolve (nothing mutated)
	//   - would_create_cycle — parent == child, or parent is a descendant of child
	//   - missing_root_component — the child or parent has no root component, so
	//                              AttachToActor would silently no-op
	Registry.Register(
		TEXT("unreal_open_mcp_actor_set_parent"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString ChildRef = Args->HasTypedField<EJson::String>(TEXT("actor"))
				? Args->GetStringField(TEXT("actor"))
				: FString();
			const FString ParentRef = Args->HasTypedField<EJson::String>(TEXT("parent"))
				? Args->GetStringField(TEXT("parent"))
				: FString();
			if (ChildRef.IsEmpty() || ParentRef.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("Both 'actor' (child) and 'parent' are required."));
			}
			const bool bKeepWorld = Args->HasTypedField<EJson::Boolean>(TEXT("keepWorldTransform"))
				? Args->GetBoolField(TEXT("keepWorldTransform"))
				: true;

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			// Resolve both actors BEFORE the transaction so a miss leaves no undo
			// entry (same resolve-before-mutate invariant as actor_create's parent).
			AActor* Child = FUnrealOpenMcpObjectRef::ResolveActor(ChildRef, World);
			if (Child == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("actor_not_found"),
					FString::Printf(
						TEXT("Actor '%s' was not found; nothing was modified."),
						*ChildRef));
			}
			AActor* Parent = FUnrealOpenMcpObjectRef::ResolveActor(ParentRef, World);
			if (Parent == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("parent_not_found"),
					FString::Printf(
						TEXT("Parent actor '%s' was not found; nothing was modified."),
						*ParentRef));
			}

			// Self-parent and cycle guard up front. AttachToActor would silently
			// drop a cycle-forming attach (the engine's attachment graph rejects
			// it), so we reject explicitly with a structured code an agent can
			// branch on. IsAttachedTo walks the attachment ancestry of Parent; if
			// Child is an ancestor of Parent, attaching Parent-under-Child's-new-
			// child-slot would form a cycle. (Mirrors Unity's upward-walk cycle
			// detection, adapted to Unreal's IsAttachedTo helper.)
			if (Parent == Child)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("would_create_cycle"),
					TEXT("Cannot attach an actor to itself."));
			}
			if (Parent->IsAttachedTo(Child))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("would_create_cycle"),
					FString::Printf(
						TEXT("Cannot attach '%s' under '%s': the parent is already a descendant of the child (would create a cycle)."),
						*Child->GetActorLabel(), *Parent->GetActorLabel()));
			}

			// Root-component gate: AttachToActor routes through the child's root
			// scene component and attaches to the parent actor (which itself
			// attaches the parent's root). Either side missing a root silently
			// no-ops, so surface it up front rather than report success.
			if (Child->GetRootComponent() == nullptr || Parent->GetRootComponent() == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_root_component"),
					TEXT("Both the child and the parent must have a root component to attach."));
			}

			const FScopedTransaction Transaction(
				NSLOCTEXT("UnrealOpenMcp", "ActorSetParent", "MCP: Set Actor Parent"));

			Child->AttachToActor(
				Parent,
				bKeepWorld ? FAttachmentTransformRules::KeepWorldTransform : FAttachmentTransformRules::KeepRelativeTransform);

			// Verify the attach took. AttachToActor returns void and can silently
			// no-op on an engine-side rejection; cancel the (otherwise empty)
			// transaction and surface a structured error so a false success is
			// never reported. (Behavior reference: UnrealMcpActorTools.cpp.)
			if (Child->GetAttachParentActor() != Parent)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("attach_failed"),
					FString::Printf(
						TEXT("Engine rejected attaching '%s' to '%s'."),
						*Child->GetActorLabel(), *Parent->GetActorLabel()));
			}

			// Dirty the level so the editor's save prompt fires.
			if (ULevel* Level = Child->GetLevel())
			{
				Level->MarkPackageDirty();
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("actor"), ToActorData(Child, /*bIncludeComponents=*/true));
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// unreal_open_mcp_actor_duplicate — duplicate an actor in the current editor
	// level. Re-spawns via SpawnActor with the source as the template (the
	// behavior reference's chosen path — it stays headless-safe and copies the
	// source's component state into the clone). Optionally renames the clone and
	// attaches it to a parent. Returns the clone's ActorData so the agent can
	// chain modify / tree tools without a second lookup.
	//
	// Wrapped in FScopedTransaction for editor Undo. `paths_hint` + `gate` are
	// forward-compat (no-op until P3.5). Structured errors:
	//   - missing_parameter — `actor` absent/empty
	//   - no_editor_world   — no GEditor / editor world
	//   - actor_not_found   — source ref did not resolve (nothing spawned)
	//   - parent_not_found  — parent ref did not resolve (nothing spawned)
	//   - spawn_failed      — SpawnActor returned null
	Registry.Register(
		TEXT("unreal_open_mcp_actor_duplicate"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString SourceRef = Args->HasTypedField<EJson::String>(TEXT("actor"))
				? Args->GetStringField(TEXT("actor"))
				: FString();
			if (SourceRef.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'actor' (the source to duplicate) is required."));
			}

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			// Resolve source + parent BEFORE spawning. A miss returns a structured
			// error with nothing spawned (no orphan actor, no undo entry).
			AActor* Source = FUnrealOpenMcpObjectRef::ResolveActor(SourceRef, World);
			if (Source == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("actor_not_found"),
					FString::Printf(
						TEXT("Source actor '%s' was not found; nothing was duplicated."),
						*SourceRef));
			}
			const FString ParentRef = Args->HasTypedField<EJson::String>(TEXT("parent"))
				? Args->GetStringField(TEXT("parent"))
				: FString();
			AActor* Parent = nullptr;
			if (!ParentRef.IsEmpty())
			{
				Parent = FUnrealOpenMcpObjectRef::ResolveActor(ParentRef, World);
				if (Parent == nullptr)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("parent_not_found"),
						FString::Printf(
							TEXT("Parent actor '%s' was not found; nothing was duplicated."),
							*ParentRef));
				}
			}

			const FString Label = Args->HasTypedField<EJson::String>(TEXT("name"))
				? Args->GetStringField(TEXT("name"))
				: FString();
			// An optional world-space offset applied to the clone's location. The
			// behavior reference duplicates in-place (offset 0); exposing it as an
			// arg lets an agent avoid stacking the clone on the source without a
			// second actor_modify call. Defaults to a small +Z nudge so the clone
			// is visible in the viewport even when the caller omitted it.
			const FVector Offset = ReadVectorField(Args, TEXT("offset"), FVector(0.0, 0.0, 0.0));

			const FScopedTransaction Transaction(
				NSLOCTEXT("UnrealOpenMcp", "ActorDuplicate", "MCP: Duplicate Actor"));

			// Spawn-from-template: SpawnParams.Template = Source makes SpawnActor
			// copy the source's component state into the clone (the behavior
			// reference's chosen duplication path — stays headless-safe and avoids
			// the EditorActorSubsystem dependency). RF_Transactional makes the
			// clone undo-trackable.
			FActorSpawnParameters SpawnParams;
			SpawnParams.Template = Source;
			SpawnParams.ObjectFlags |= RF_Transactional;
			const FTransform DupTransform(
				Source->GetActorRotation(),
				Source->GetActorLocation() + Offset,
				Source->GetActorScale3D());
			AActor* Clone = World->SpawnActor<AActor>(Source->GetClass(), DupTransform, SpawnParams);
			if (Clone == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("spawn_failed"),
					FString::Printf(
						TEXT("SpawnActor returned null while duplicating '%s'."),
						*Source->GetActorLabel()));
			}

			// SetActorLabelUnique (not SetActorLabel): a user-supplied label that
			// collides with the source's label (the default!) would otherwise make
			// both ambiguous to ResolveActor. Empty label → keep the engine's
			// auto-generated copy (which is the source label verbatim), so we
			// force uniqueness even then.
			FActorLabelUtilities::SetActorLabelUnique(
				Clone,
				Label.IsEmpty() ? Source->GetActorLabel() : Label);

			// Optional parent attach. Same warning contract as actor_create: if
			// the attach silently no-ops (rootless clone), surface a warning in
			// the result rather than fail and leak the clone.
			FString AttachWarning;
			if (Parent != nullptr)
			{
				Clone->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
				if (Clone->GetAttachParentActor() != Parent)
				{
					AttachWarning = FString::Printf(
						TEXT(" (warning: could not attach to parent '%s' — the clone has no root component)"),
						*Parent->GetActorLabel());
				}
			}

			if (ULevel* Level = Clone->GetLevel())
			{
				Level->MarkPackageDirty();
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("actor"), ToActorData(Clone, /*bIncludeComponents=*/true));
			if (!AttachWarning.IsEmpty())
			{
				Result->SetStringField(TEXT("warning"), AttachWarning);
			}
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// unreal_open_mcp_actor_destroy — destroy one or more actors in the current
	// editor level. Single (`actor`) or batch (`actors` array); the batch path
	// resolves every target BEFORE opening the transaction so a miss returns
	// actor_not_found with nothing destroyed (no partial batch). Each destroy
	// goes through EditorDestroyActor (the editor path — records into the
	// transaction buffer and updates the level's actor list); World->DestroyActor
	// would bypass the editor's selection/outliner bookkeeping.
	//
	// Wrapped in FScopedTransaction for editor Undo. `paths_hint` + `gate` are
	// forward-compat (no-op until P3.5). Structured errors:
	//   - missing_parameter — neither `actor` nor `actors` supplied
	//   - no_editor_world   — no GEditor / editor world
	//   - actor_not_found   — a ref did not resolve (nothing destroyed)
	//   - destroy_failed    — EditorDestroyActor returned false for a target
	Registry.Register(
		TEXT("unreal_open_mcp_actor_destroy"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			// Collect target refs. `actor` (single) and `actors` (batch) mirror
			// actor_modify's targeting pair. An empty set is missing_parameter.
			TArray<FString> Refs;
			if (Args->HasTypedField<EJson::String>(TEXT("actor")))
			{
				Refs.Add(Args->GetStringField(TEXT("actor")));
			}
			if (Args->HasTypedField<EJson::Array>(TEXT("actors")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
				Args->TryGetArrayField(TEXT("actors"), Arr);
				if (Arr != nullptr)
				{
					for (const TSharedPtr<FJsonValue>& V : *Arr)
					{
						if (V.IsValid() && V->Type == EJson::String)
						{
							const FString S = V->AsString();
							if (!S.IsEmpty())
							{
								Refs.Add(S);
							}
						}
					}
				}
			}
			if (Refs.Num() == 0)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("Either 'actor' (string) or 'actors' (array of strings) is required."));
			}

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			// Resolve every target BEFORE the transaction (resolve-before-mutate).
			// A miss returns actor_not_found with nothing destroyed — opening the
			// transaction first would push an empty entry onto the undo stack and
			// a later miss would leave a partial destroy with no clean rollback.
			TArray<AActor*> Targets;
			Targets.Reserve(Refs.Num());
			for (const FString& Ref : Refs)
			{
				AActor* Resolved = FUnrealOpenMcpObjectRef::ResolveActor(Ref, World);
				if (Resolved == nullptr)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("actor_not_found"),
						FString::Printf(
							TEXT("Actor '%s' was not found; nothing was destroyed."),
							*Ref));
				}
				Targets.Add(Resolved);
			}

			const FScopedTransaction Transaction(
				NSLOCTEXT("UnrealOpenMcp", "ActorDestroy", "MCP: Destroy Actor"));

			TArray<FString> DestroyedLabels;
			for (AActor* Actor : Targets)
			{
				const FString Label = Actor->GetActorLabel();
				// EditorDestroyActor is the editor path: it records the destroy
				// into the transaction buffer, updates the level's actor list, and
				// fires the editor's selection/outliner refresh hooks. The
				// bShouldModifyLevel=true flag propagates the dirty bit. Returns
				// false on rejection (e.g. an actor the editor refuses to remove).
				if (World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true))
				{
					DestroyedLabels.Add(Label);
				}
				else
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("destroy_failed"),
						FString::Printf(
							TEXT("EditorDestroyActor returned false for '%s'."),
							*Label));
				}
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> DestroyedValues;
			DestroyedValues.Reserve(DestroyedLabels.Num());
			for (const FString& L : DestroyedLabels)
			{
				DestroyedValues.Add(MakeShared<FJsonValueString>(L));
			}
			Result->SetArrayField(TEXT("destroyed"), DestroyedValues);
			Result->SetNumberField(TEXT("count"), DestroyedLabels.Num());
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// unreal_open_mcp_actor_component_add — add a component to an actor in the
	// current editor level. Resolves the component class (same ResolveClass
	// surface as actor_create), validates it is a non-abstract UActorComponent,
	// creates it via NewObject, and runs the registration sequence
	// (AddInstanceComponent → scene attach/root-set → OnComponentCreated →
	// RegisterComponent) so the new component is live in the editor. Returns the
	// new component's ComponentData (name + class + properties) so the agent can
	// chain component_modify without a second lookup.
	//
	// Wrapped in FScopedTransaction for editor Undo. `paths_hint` + `gate` are
	// forward-compat (no-op until P3.5). Structured errors:
	//   - missing_parameter — `actor` or `componentClass` absent/empty
	//   - no_editor_world   — no GEditor / editor world
	//   - actor_not_found   — actor ref did not resolve (nothing added)
	//   - class_not_found   — componentClass did not resolve to a UClass
	//   - invalid_parameter — resolved class is not a UActorComponent / abstract
	//   - name_conflict     — a component with the requested name already exists
	//   - create_failed     — NewObject returned null
	Registry.Register(
		TEXT("unreal_open_mcp_actor_component_add"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString ActorRef = Args->HasTypedField<EJson::String>(TEXT("actor"))
				? Args->GetStringField(TEXT("actor"))
				: FString();
			if (ActorRef.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'actor' is required."));
			}
			const FString ClassRef = Args->HasTypedField<EJson::String>(TEXT("componentClass"))
				? Args->GetStringField(TEXT("componentClass"))
				: FString();
			FString Code;
			FString Message;
			UClass* Class = ResolveComponentClass(ClassRef, Code, Message);
			if (Class == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(Code, Message);
			}
			const FString NameArg = Args->HasTypedField<EJson::String>(TEXT("name"))
				? Args->GetStringField(TEXT("name"))
				: FString();

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			AActor* Actor = FUnrealOpenMcpObjectRef::ResolveActor(ActorRef, World);
			if (Actor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("actor_not_found"),
					FString::Printf(
						TEXT("Actor '%s' was not found; nothing was added."),
						*ActorRef));
			}

			// Resolve the component name BEFORE NewObject. A user-supplied name
			// that collides with an existing component under the actor would trip
			// a StaticAllocateObject fatal assert inside NewObject; resolve it up
			// front and return a structured name_conflict. (Behavior reference:
			// UnrealMcpActorTools.cpp.) An empty NameArg → a unique object name
			// derived from the class name (the engine default).
			FName CompName;
			if (NameArg.IsEmpty())
			{
				CompName = MakeUniqueObjectName(Actor, Class, Class->GetFName());
			}
			else
			{
				CompName = FName(*NameArg);
				if (StaticFindObjectFast(nullptr, Actor, CompName) != nullptr)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("name_conflict"),
						FString::Printf(
							TEXT("A component named '%s' already exists on actor '%s'."),
							*NameArg, *Actor->GetActorLabel()));
				}
			}

			const FScopedTransaction Transaction(
				NSLOCTEXT("UnrealOpenMcp", "ComponentAdd", "MCP: Add Component"));

			// Snapshot the actor's component list BEFORE mutating it so the add
			// is undo-able as a single step with the NewObject allocation.
			Actor->Modify();
			UActorComponent* NewComp = NewObject<UActorComponent>(Actor, Class, CompName, RF_Transactional);
			if (NewComp == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("create_failed"),
					FString::Printf(
						TEXT("NewObject returned null for componentClass '%s'."),
						*ClassRef));
			}

			// Registration sequence (behavior reference: UnrealMcpActorTools.cpp):
			// AddInstanceComponent makes the component visible in the editor's
			// component tree; a USceneComponent attaches to the actor's root (or
			// becomes the root when the actor has none); OnComponentCreated +
			// RegisterComponent bring it live. Order matters — attaching before
			// RegisterComponent lets the registration pick up the attachment.
			Actor->AddInstanceComponent(NewComp);
			if (USceneComponent* SceneComp = Cast<USceneComponent>(NewComp))
			{
				if (USceneComponent* Root = Actor->GetRootComponent())
				{
					SceneComp->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
				}
				else
				{
					Actor->SetRootComponent(SceneComp);
				}
			}
			NewComp->OnComponentCreated();
			NewComp->RegisterComponent();
			Actor->MarkPackageDirty();

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(
				TEXT("component"), ToComponentData(NewComp, /*bIncludeProperties=*/true));
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// unreal_open_mcp_actor_component_destroy — destroy a component on an actor
	// in the current editor level. Only INSTANCE components (the ones the editor
	// added at edit time, surfaced via AddInstanceComponent) can be destroyed
	// cleanly; native/archetype components are rejected explicitly so the caller
	// gets a clear reason, not a half-removed component. The destroy runs
	// RemoveInstanceComponent → DestroyComponent and dirties the actor's package.
	//
	// Wrapped in FScopedTransaction for editor Undo. `paths_hint` + `gate` are
	// forward-compat (no-op until P3.5). Structured errors:
	//   - missing_parameter — `actor` or `component` absent/empty
	//   - no_editor_world   — no GEditor / editor world
	//   - actor_not_found   — actor ref did not resolve (nothing destroyed)
	//   - component_not_found — component ref did not resolve on the actor
	//   - not_instance_component — the component is native/archetype and cannot
	//                              be destroyed through this path
	Registry.Register(
		TEXT("unreal_open_mcp_actor_component_destroy"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString ActorRef = Args->HasTypedField<EJson::String>(TEXT("actor"))
				? Args->GetStringField(TEXT("actor"))
				: FString();
			const FString CompRef = Args->HasTypedField<EJson::String>(TEXT("component"))
				? Args->GetStringField(TEXT("component"))
				: FString();
			if (ActorRef.IsEmpty() || CompRef.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("Both 'actor' and 'component' are required."));
			}

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			AActor* Actor = FUnrealOpenMcpObjectRef::ResolveActor(ActorRef, World);
			if (Actor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("actor_not_found"),
					FString::Printf(
						TEXT("Actor '%s' was not found; nothing was destroyed."),
						*ActorRef));
			}
			UActorComponent* Component = FUnrealOpenMcpObjectRef::ResolveComponent(Actor, CompRef);
			if (Component == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("component_not_found"),
					FString::Printf(
						TEXT("Component '%s' was not found on actor '%s'."),
						*CompRef, *Actor->GetActorLabel()));
			}

			// Instance-component gate: only components the editor added at edit
			// time (surfaced via AddInstanceComponent, visible in the editor's
			// component tree) can be destroyed cleanly. A native/archetype
			// component (the root of a StaticMeshActor, a Character's Movement
			// component) would leave the actor in a broken half-state; reject
			// explicitly with a structured code.
			if (!Actor->GetInstanceComponents().Contains(Component))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("not_instance_component"),
					FString::Printf(
						TEXT("Component '%s' on actor '%s' is not an instance component (native/archetype components cannot be destroyed this way)."),
						*Component->GetName(), *Actor->GetActorLabel()));
			}

			const FScopedTransaction Transaction(
				NSLOCTEXT("UnrealOpenMcp", "ComponentDestroy", "MCP: Destroy Component"));

			// RemoveInstanceComponent detaches the component from the editor's
			// component tree BEFORE DestroyComponent tears it down — the order
			// avoids a dangling pointer in the editor's instance-components array.
			Actor->Modify();
			Actor->RemoveInstanceComponent(Component);
			Component->DestroyComponent();
			Actor->MarkPackageDirty();

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("destroyed"), true);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// unreal_open_mcp_actor_component_get — read a single component on an actor.
	// Read-only (no gate, no transaction). Returns the component's ComponentData
	// (name + class + a properties object reflected from the component's Edit/
	// Blueprint-visible FProperties) so an agent can inspect state before
	// modifying it. The properties object is the read counterpart of the
	// component_modify write bag (UStructToJsonObject vs JsonValueToUProperty).
	//
	// Structured errors:
	//   - missing_parameter — `actor` or `component` absent/empty
	//   - no_editor_world   — no GEditor / editor world
	//   - actor_not_found   — actor ref did not resolve
	//   - component_not_found — component ref did not resolve on the actor
	Registry.Register(
		TEXT("unreal_open_mcp_actor_component_get"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString ActorRef = Args->HasTypedField<EJson::String>(TEXT("actor"))
				? Args->GetStringField(TEXT("actor"))
				: FString();
			const FString CompRef = Args->HasTypedField<EJson::String>(TEXT("component"))
				? Args->GetStringField(TEXT("component"))
				: FString();
			if (ActorRef.IsEmpty() || CompRef.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("Both 'actor' and 'component' are required."));
			}

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			AActor* Actor = FUnrealOpenMcpObjectRef::ResolveActor(ActorRef, World);
			if (Actor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("actor_not_found"),
					FString::Printf(
						TEXT("Actor '%s' was not found."),
						*ActorRef));
			}
			UActorComponent* Component = FUnrealOpenMcpObjectRef::ResolveComponent(Actor, CompRef);
			if (Component == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("component_not_found"),
					FString::Printf(
						TEXT("Component '%s' was not found on actor '%s'."),
						*CompRef, *Actor->GetActorLabel()));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(
				TEXT("component"), ToComponentData(Component, /*bIncludeProperties=*/true));
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	// unreal_open_mcp_actor_component_modify — write reflected FProperty values
	// on a resolved component. Same flat `properties` bag + same shared
	// ApplyProperties helper as actor_modify / object_modify; the scene-component
	// relative-transform shortcuts (relativeLocation / relativeRotation /
	// relativeScale3D) are routed to the component's SetRelative* APIs by the
	// helper. Per-field errors accumulate in `errors[]` and do NOT abort the
	// batch (partial success is the norm).
	//
	// Wrapped in FScopedTransaction for editor Undo. `paths_hint` + `gate` are
	// forward-compat (no-op until P3.5). Structured errors:
	//   - missing_parameter — `actor`, `component`, or `properties` absent
	//   - no_editor_world   — no GEditor / editor world
	//   - actor_not_found   — actor ref did not resolve (nothing mutated)
	//   - component_not_found — component ref did not resolve (nothing mutated)
	Registry.Register(
		TEXT("unreal_open_mcp_actor_component_modify"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString ActorRef = Args->HasTypedField<EJson::String>(TEXT("actor"))
				? Args->GetStringField(TEXT("actor"))
				: FString();
			const FString CompRef = Args->HasTypedField<EJson::String>(TEXT("component"))
				? Args->GetStringField(TEXT("component"))
				: FString();
			if (ActorRef.IsEmpty() || CompRef.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("Both 'actor' and 'component' are required."));
			}
			if (!Args->HasTypedField<EJson::Object>(TEXT("properties")))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'properties' object is required."));
			}
			const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
			Args->TryGetObjectField(TEXT("properties"), PropsPtr);
			if (PropsPtr == nullptr || !PropsPtr->IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("'properties' must be a JSON object of name -> value pairs."));
			}
			const TSharedPtr<FJsonObject>& Properties = *PropsPtr;

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			AActor* Actor = FUnrealOpenMcpObjectRef::ResolveActor(ActorRef, World);
			if (Actor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("actor_not_found"),
					FString::Printf(
						TEXT("Actor '%s' was not found; nothing was modified."),
						*ActorRef));
			}
			UActorComponent* Component = FUnrealOpenMcpObjectRef::ResolveComponent(Actor, CompRef);
			if (Component == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("component_not_found"),
					FString::Printf(
						TEXT("Component '%s' was not found on actor '%s'; nothing was modified."),
						*CompRef, *Actor->GetActorLabel()));
			}

			const FScopedTransaction Transaction(
				NSLOCTEXT("UnrealOpenMcp", "ComponentModify", "MCP: Modify Component"));

			TArray<FString> Errors;
			const int32 Applied = FUnrealOpenMcpPropertyJson::ApplyProperties(Component, Properties, Errors);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("applied"), Applied);
			Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
			Result->SetStringField(TEXT("component"), Component->GetName());
			if (Errors.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> ErrorValues;
				ErrorValues.Reserve(Errors.Num());
				for (const FString& E : Errors)
				{
					ErrorValues.Add(MakeShared<FJsonValueString>(E));
				}
				Result->SetArrayField(TEXT("errors"), ErrorValues);
			}
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// unreal_open_mcp_actor_component_list_all — list every component on an
	// actor. Read-only (no gate, no transaction). Returns a components[] array
	// of ComponentData (name + class; properties omitted to save tokens — use
	// component_get for the full property dump of one component). Mirrors
	// Unity's component-list surface (the host-bound variant; the engine-wide
	// type catalog is a later-phase tool).
	//
	// Structured errors:
	//   - missing_parameter — `actor` absent/empty
	//   - no_editor_world   — no GEditor / editor world
	//   - actor_not_found   — actor ref did not resolve
	Registry.Register(
		TEXT("unreal_open_mcp_actor_component_list_all"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString ActorRef = Args->HasTypedField<EJson::String>(TEXT("actor"))
				? Args->GetStringField(TEXT("actor"))
				: FString();
			if (ActorRef.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'actor' is required."));
			}

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			AActor* Actor = FUnrealOpenMcpObjectRef::ResolveActor(ActorRef, World);
			if (Actor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("actor_not_found"),
					FString::Printf(
						TEXT("Actor '%s' was not found."),
						*ActorRef));
			}

			TArray<TSharedPtr<FJsonValue>> Components;
			for (UActorComponent* Component : Actor->GetComponents())
			{
				if (Component == nullptr)
				{
					continue;
				}
				// Properties omitted per entry — a full property dump of every
				// component would blow the token budget on a complex actor. Use
				// component_get for the full property object of one component.
				Components.Add(MakeShared<FJsonValueObject>(
					ToComponentData(Component, /*bIncludeProperties=*/false)));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("components"), Components);
			Result->SetNumberField(TEXT("count"), Components.Num());
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	UE_LOG(
		LogUnrealOpenMcp,
		Log,
		TEXT("[Unreal Open MCP] actor tools registered: unreal_open_mcp_actor_find, unreal_open_mcp_actor_create, unreal_open_mcp_actor_modify, unreal_open_mcp_object_modify, unreal_open_mcp_actor_set_parent, unreal_open_mcp_actor_duplicate, unreal_open_mcp_actor_destroy, unreal_open_mcp_actor_component_add, unreal_open_mcp_actor_component_destroy, unreal_open_mcp_actor_component_get, unreal_open_mcp_actor_component_modify, unreal_open_mcp_actor_component_list_all"));
}
