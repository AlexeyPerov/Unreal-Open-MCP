// FProperty <-> JSON write helper — see header for the transform-shortcut
// semantics and the writable-property gate.
//
// Behavior adapted from Unreal-MCP's FUnrealMcpPropertyJson::ApplyProperties
// (read-only reference). That project routes the same helper from a Runtime
// module so it also works in a packaged game; this port lives in the editor
// module (the mutate family lives here), so the WITH_EDITOR guard around
// Modify/PreEditChange/PostEditChange is belt-and-suspenders rather than
// load-bearing — the editor module is never compiled into a packaged game.
// The transform terminal handling, the case-insensitive property lookup, the
// explicit read-only rejection, and the FJsonObjectConverter write are
// preserved verbatim because they are the load-bearing correctness contract
// (an empty {} must not count as applied; a read-only prop must error, not
// silently no-op).
#include "Tools/UnrealOpenMcpPropertyJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"   // FJsonObjectConverter::JsonValueToUProperty
#include "GameFramework/Actor.h"   // SetActorLocation/Rotation/Scale3D
#include "Components/SceneComponent.h"   // SetRelativeLocation/Rotation/Scale3D
#include "UObject/UnrealType.h"    // FProperty, TFieldIterator
#include "UObject/Class.h"         // UStruct for FindPropertyByJsonName

namespace
{
	/**
	 * Read a {x,y,z} object into an FVector. Returns true only when at least
	 * one recognized axis key was present — an empty or typo'd object must NOT
	 * count as an applied no-op write (the scene-component RelativeLocation
	 * FProperty exists and JsonValueToUProperty returns true for {}, so falling
	 * through would silently count a no-op as a write).
	 */
	bool JsonToVector(const TSharedPtr<FJsonValue>& Value, FVector& Out)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj->IsValid())
		{
			return false;
		}
		bool bAny = false;
		double C = 0.0;
		if ((*Obj)->TryGetNumberField(TEXT("x"), C)) { Out.X = C; bAny = true; }
		if ((*Obj)->TryGetNumberField(TEXT("y"), C)) { Out.Y = C; bAny = true; }
		if ((*Obj)->TryGetNumberField(TEXT("z"), C)) { Out.Z = C; bAny = true; }
		return bAny;
	}

	/**
	 * Read a {pitch,yaw,roll} object into an FRotator. Same terminal semantics
	 * as JsonToVector — returns true only when at least one recognized key was
	 * present so an empty object never counts as an applied write.
	 */
	bool JsonToRotator(const TSharedPtr<FJsonValue>& Value, FRotator& Out)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj->IsValid())
		{
			return false;
		}
		bool bAny = false;
		double C = 0.0;
		if ((*Obj)->TryGetNumberField(TEXT("pitch"), C)) { Out.Pitch = C; bAny = true; }
		if ((*Obj)->TryGetNumberField(TEXT("yaw"), C)) { Out.Yaw = C; bAny = true; }
		if ((*Obj)->TryGetNumberField(TEXT("roll"), C)) { Out.Roll = C; bAny = true; }
		return bAny;
	}

	/**
	 * Find a reflected FProperty by its JSON key. Property names in Unreal are
	 * often lower-first-cased on the wire (FJsonObjectConverter standardizes
	 * the first character to lowercase), so the match is case-insensitive — a
	 * loosely-cased LLM-supplied key still resolves.
	 */
	FProperty* FindPropertyByJsonName(const UStruct* Struct, const FString& Key)
	{
		if (Struct == nullptr)
		{
			return nullptr;
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (It->GetName().Equals(Key, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}
}

namespace FUnrealOpenMcpPropertyJson
{
	int32 ApplyProperties(
		UObject* Object,
		const TSharedPtr<FJsonObject>& Properties,
		TArray<FString>& OutErrors)
	{
		if (Object == nullptr || !Properties.IsValid())
		{
			return 0;
		}

		AActor* AsActor = Cast<AActor>(Object);
		USceneComponent* AsScene = Cast<USceneComponent>(Object);

		// Snapshot the pre-change state into the transaction buffer BEFORE
		// mutating. Calling Modify() after the writes would record the
		// already-changed values, making undo a no-op. PreEditChange(nullptr)
		// is the front half of the standard editor edit protocol (Pre → write
		// → Post below): properties whose edit hooks tear down state up front
		// (component render-state/registration guards, cached-data
		// invalidation) can misbehave when raw memory is written without the
		// pre-notify. Both are WITH_EDITOR-only; in a packaged game the raw
		// FProperty writes below still apply (no transaction buffer or edit
		// hooks exist there to drive).
#if WITH_EDITOR
		Object->Modify();
		Object->PreEditChange(nullptr);
#endif

		int32 Applied = 0;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Properties->Values)
		{
			const FString& Key = Field.Key;
			const TSharedPtr<FJsonValue>& Value = Field.Value;

			// --- Actor transform shortcuts (not single FProperties). ---
			// Handled TERMINALLY: on a parse success we apply and count it; on
			// a parse failure (empty {} or all-typo'd keys) we record an error
			// and continue. Falling through to the generic FProperty path would
			// either count an empty-object no-op as applied (the actor has no
			// `location` FProperty, but scene components do have
			// RelativeLocation and JsonValueToUProperty returns true for {}) or
			// emit a misleading "unknown property" for the actor pseudo-keys.
			if (AsActor != nullptr)
			{
				if (Key.Equals(TEXT("location"), ESearchCase::IgnoreCase))
				{
					FVector V = AsActor->GetActorLocation();
					if (JsonToVector(Value, V)) { AsActor->SetActorLocation(V); ++Applied; }
					else OutErrors.Add(FString::Printf(TEXT("'%s' expects a {x,y,z} object with at least one numeric axis"), *Key));
					continue;
				}
				if (Key.Equals(TEXT("rotation"), ESearchCase::IgnoreCase))
				{
					FRotator R = AsActor->GetActorRotation();
					if (JsonToRotator(Value, R)) { AsActor->SetActorRotation(R); ++Applied; }
					else OutErrors.Add(FString::Printf(TEXT("'%s' expects a {pitch,yaw,roll} object with at least one numeric key"), *Key));
					continue;
				}
				if (Key.Equals(TEXT("scale"), ESearchCase::IgnoreCase))
				{
					FVector S = AsActor->GetActorScale3D();
					if (JsonToVector(Value, S)) { AsActor->SetActorScale3D(S); ++Applied; }
					else OutErrors.Add(FString::Printf(TEXT("'%s' expects a {x,y,z} object with at least one numeric axis"), *Key));
					continue;
				}
			}

			// --- Scene-component relative-transform shortcuts. ---
			if (AsScene != nullptr)
			{
				if (Key.Equals(TEXT("relativeLocation"), ESearchCase::IgnoreCase))
				{
					FVector V = AsScene->GetRelativeLocation();
					if (JsonToVector(Value, V)) { AsScene->SetRelativeLocation(V); ++Applied; }
					else OutErrors.Add(FString::Printf(TEXT("'%s' expects a {x,y,z} object with at least one numeric axis"), *Key));
					continue;
				}
				if (Key.Equals(TEXT("relativeRotation"), ESearchCase::IgnoreCase))
				{
					FRotator R = AsScene->GetRelativeRotation();
					if (JsonToRotator(Value, R)) { AsScene->SetRelativeRotation(R); ++Applied; }
					else OutErrors.Add(FString::Printf(TEXT("'%s' expects a {pitch,yaw,roll} object with at least one numeric key"), *Key));
					continue;
				}
				if (Key.Equals(TEXT("relativeScale3D"), ESearchCase::IgnoreCase)
					|| Key.Equals(TEXT("relativeScale"), ESearchCase::IgnoreCase))
				{
					FVector S = AsScene->GetRelativeScale3D();
					if (JsonToVector(Value, S)) { AsScene->SetRelativeScale3D(S); ++Applied; }
					else OutErrors.Add(FString::Printf(TEXT("'%s' expects a {x,y,z} object with at least one numeric axis"), *Key));
					continue;
				}
			}

			// --- Generic reflected FProperty write. ---
			FProperty* Prop = FindPropertyByJsonName(Object->GetClass(), Key);
			if (Prop == nullptr)
			{
				OutErrors.Add(FString::Printf(TEXT("unknown property '%s'"), *Key));
				continue;
			}
			// Writable iff editable-and-not-const, OR blueprint-visible-and-not-
			// blueprint-read-only. Without this gate, a BlueprintReadOnly prop
			// (CPF_BlueprintVisible set, no CPF_Edit, no CPF_EditConst) would be
			// silently written and then reverted on the next save.
			const bool bEditable = Prop->HasAnyPropertyFlags(CPF_Edit)
				&& !Prop->HasAnyPropertyFlags(CPF_EditConst);
			const bool bBlueprintWritable = Prop->HasAnyPropertyFlags(CPF_BlueprintVisible)
				&& !Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly);
			if (!bEditable && !bBlueprintWritable)
			{
				// Reject read-only properties explicitly so the caller gets a
				// clear reason, not a silent no-op.
				OutErrors.Add(FString::Printf(TEXT("property '%s' is read-only"), *Key));
				continue;
			}

			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Object);
			if (FJsonObjectConverter::JsonValueToUProperty(Value, Prop, ValuePtr, /*CheckFlags*/ 0, /*SkipFlags*/ 0))
			{
				++Applied;
			}
			else
			{
				OutErrors.Add(FString::Printf(TEXT("could not set property '%s' from the provided value"), *Key));
			}
		}

		// PostEditChange pairs the PreEditChange(nullptr) above on every path
		// (it re-registers a component and re-runs the edit hooks the
		// pre-notify tore down) — calling it only when Applied>0 would leave a
		// zero-applied object stranded mid-edit (e.g. an unregistered
		// component). Only dirty the package when something actually changed.
#if WITH_EDITOR
		Object->PostEditChange();
#endif
		if (Applied > 0)
		{
			Object->MarkPackageDirty();
		}
		return Applied;
	}
}
