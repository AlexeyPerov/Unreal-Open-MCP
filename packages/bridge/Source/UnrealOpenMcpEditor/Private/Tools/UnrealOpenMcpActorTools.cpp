// Actor-tool family — see header for the find/resolve contract and the
// targeted-miss-vs-error semantics. This file owns the `actor_find` and
// `actor_create` handlers plus the shared actor-data serializer (`ToActorData`)
// that later actor tools (modify / tree ops) will reuse for consistent output.
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
#include "UnrealOpenMcpLog.h"

#include "GameFramework/Actor.h"
#include "EngineUtils.h"                // TActorIterator
#include "Components/ActorComponent.h"
// P2.3 — FAttachmentTransformRules for AttachToActor (KeepWorldTransform keeps
// the spawn pose stable across the reparent, matching the behavior reference).
#include "Components/SceneComponent.h"
// P2.3 — actor_create wraps the spawn in FScopedTransaction so the new actor
// is undoable from the editor (Ctrl+Z). Mirrors Unity's
// Undo.RegisterCreatedObjectUndo at the transaction granularity.
#include "ScopedTransaction.h"
// P2.3 — SetActorLabelUnique avoids label collisions that would make two
// actors ambiguous to ResolveActor (label → name → path). Lives in UnrealEd.
#include "ActorEditorUtils.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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
		});

	UE_LOG(
		LogUnrealOpenMcp,
		Log,
		TEXT("[Unreal Open MCP] actor tools registered: unreal_open_mcp_actor_find, unreal_open_mcp_actor_create"));
}
