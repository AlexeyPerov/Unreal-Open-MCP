// Actor-tool family — see header for the find/resolve contract and the
// targeted-miss-vs-error semantics. This file owns the `actor_find` handler
// plus the shared actor-data serializer (`ToActorData`) that later actor tools
// (modify / create / tree ops) will reuse for consistent output.
//
// Arg parsing: the handler receives the raw POST body FString (the registry
// contract is raw-body; each tool owns its arg extraction). P2.2 is the first
// typed tool that needs structured args, so it parses the body into an
// FJsonObject via the Json module (TJsonReader + FJsonSerializer). The
// registry/dispatch spine stays raw-body — only the handler layer parses.
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

	UE_LOG(
		LogUnrealOpenMcp,
		Log,
		TEXT("[Unreal Open MCP] actor tools registered: unreal_open_mcp_actor_find"));
}
