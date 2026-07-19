// Editor-application + selection tool family — see header for the pending/poll
// contract, the explicit-clear-required rule, and the game-thread contract.
//
// This file owns four handlers:
//   - editor_application_get_state (read-only PIE / editor snapshot)
//   - editor_application_set_state (mutating; start/stop/pause/resume)
//   - editor_selection_get         (read-only selected-actor roster)
//   - editor_selection_set         (mutating; replace-by-refs or explicit clear)
//
// Arg parsing / output: same contract as the actor family — each handler
// receives the raw POST body FString and parses it into an FJsonObject (Json
// module), and builds pre-serialized JSON output. Actor identity refs reuse the
// { name, label, class, path } shape from the actor family so a selection read
// chains straight into actor_modify / screenshot / inspect.
#include "Tools/UnrealOpenMcpEditorTools.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Tools/UnrealOpenMcpObjectRef.h"
#include "UnrealOpenMcpLog.h"

#include "Editor.h"                     // GEditor + FRequestPlaySessionParams
#include "Engine/Selection.h"           // USelection + FSelectionIterator
#include "Engine/World.h"               // UWorld::IsPaused / GetMapName
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"  // APlayerController::SetPause
#include "ScopedTransaction.h"          // selection_set groups as one undo

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "UnrealOpenMcpEditorTools"

namespace
{
	/**
	 * Parse the raw POST body into a JSON object. An empty body is a valid
	 * "no args" call → empty object (handlers resolve optional fields with
	 * defaults). A non-empty body that is not a JSON object → null (the caller
	 * surfaces a structured invalid_parameter). Mirrors the actor family's
	 * ParseBody so the parsing contract is identical across tool families.
	 */
	TSharedPtr<FJsonObject> ParseBody(const FString& Body)
	{
		const FString Trimmed = Body.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
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
	 * Serialize one actor into the identity ref the P2 actor family emits:
	 * { name, label, class, path }. Same field vocabulary as ToActorData's
	 * identity block so a selection read is drop-in with actor_find output.
	 */
	TSharedRef<FJsonObject> ToActorIdentity(AActor* Actor)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Actor->GetName());
		Json->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Json->SetStringField(
			TEXT("class"),
			Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
		Json->SetStringField(TEXT("path"), Actor->GetPathName());
		return Json;
	}

	// ------------------------------------------------------------------------
	// editor_application_get_state helpers
	// ------------------------------------------------------------------------

	/** True when a PIE / SIE session world exists (playing OR simulating). */
	bool IsPlaySessionActive()
	{
		return GEditor != nullptr && GEditor->PlayWorld != nullptr;
	}

	/** True when the active play session is Simulate-In-Editor (SIE). */
	bool IsSimulating()
	{
		return GEditor != nullptr
			&& GEditor->PlayWorld != nullptr
			&& GEditor->bIsSimulatingInEditor;
	}

	/** True when a real Play-In-Editor session (not SIE) is running. */
	bool IsPlaying()
	{
		return IsPlaySessionActive() && !GEditor->bIsSimulatingInEditor;
	}

	/** True when the PIE world is currently paused. UWorld::IsPaused reflects
	 *  the world settings' pauser player state, which SetPause sets. */
	bool IsPaused()
	{
		return IsPlaySessionActive() && GEditor->PlayWorld->IsPaused();
	}
}

void FUnrealOpenMcpEditorTools::Register(FUnrealOpenMcpToolRegistry& Registry)
{
	// =========================================================================
	// unreal_open_mcp_editor_application_get_state — read-only PIE snapshot.
	// =========================================================================
	//
	// Reports { isPlaying, isPaused, isSimulating, editorMap, editorMapName }.
	//   - isPlaying    — a real PIE session is running (PlayWorld && !SIE).
	//   - isSimulating — a Simulate-In-Editor session is running.
	//   - isPaused     — the PIE world is paused (UWorld::IsPaused).
	//   - editorMap    — the EDITOR world's persistent-level package path
	//                    (e.g. '/Game/Maps/Arena', '/Temp/Untitled'); always the
	//                    editing world, never the transient PIE world.
	//   - editorMapName— the short map name (World::GetMapName).
	// Read-only — no gate. Structured error: editor_unavailable (no GEditor /
	// editor world; e.g. a commandlet).
	Registry.Register(
		TEXT("unreal_open_mcp_editor_application_get_state"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			// Body is ignored (empty-object args), but a malformed non-empty
			// body is still a structured invalid_parameter for consistency.
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			if (GEditor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("editor_unavailable"),
					TEXT("No editor is available (GEditor is null). This tool requires the Unreal Editor."));
			}

			UWorld* EditorWorld = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (EditorWorld == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("editor_unavailable"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("isPlaying"), IsPlaying());
			Result->SetBoolField(TEXT("isPaused"), IsPaused());
			Result->SetBoolField(TEXT("isSimulating"), IsSimulating());

			// editorMap = persistent-level package path (unambiguous);
			// editorMapName = short name. GetOutermost()->GetName() is the
			// package path; GetMapName strips any PIE prefix already.
			const FString PackagePath = EditorWorld->GetOutermost()
				? EditorWorld->GetOutermost()->GetName()
				: FString();
			Result->SetStringField(TEXT("editorMap"), PackagePath);
			Result->SetStringField(TEXT("editorMapName"), EditorWorld->GetMapName());

			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	// =========================================================================
	// unreal_open_mcp_editor_application_set_state — drive PIE transitions.
	// =========================================================================
	//
	// `action` (required): start | stop | pause | resume.
	//   - start  — request a PIE session (RequestPlaySession). LATENT: the
	//              session starts on a later editor tick → { pending:true }.
	//              Refused with invalid_transition when a session is already
	//              active (no silent restart).
	//   - stop   — request the PIE session end (RequestEndPlayMap). LATENT →
	//              { pending:true }. Refused with invalid_transition when no
	//              session is active.
	//   - pause  — pause the running PIE world (PlayerController::SetPause(true)).
	//              Applies immediately → { pending:false, isPaused:true }.
	//              Refused when not playing (invalid_transition) or already
	//              paused (invalid_transition).
	//   - resume — unpause (SetPause(false)) → { pending:false, isPaused:false }.
	//              Refused when not playing or not currently paused.
	//
	// Mutating (drives the editor process). The gate's mandatory `paths_hint`
	// (the current map package and/or project scope) is enforced by the
	// dispatcher BEFORE the handler runs. Structured errors:
	//   - invalid_parameter  — malformed body / `action` not a string / unknown
	//   - missing_parameter  — `action` absent
	//   - invalid_transition — the requested transition is not valid from the
	//                          current state (already playing / not playing /
	//                          already paused / not paused)
	//   - editor_unavailable — no GEditor
	Registry.Register(
		TEXT("unreal_open_mcp_editor_application_set_state"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const bool bHasAction = Args->HasField(TEXT("action"));
			if (!bHasAction)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'action' is required (start | stop | pause | resume)."));
			}
			if (!Args->HasTypedField<EJson::String>(TEXT("action")))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("'action' must be a string (start | stop | pause | resume)."));
			}
			const FString Action = Args->GetStringField(TEXT("action")).ToLower();

			if (GEditor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("editor_unavailable"),
					TEXT("No editor is available (GEditor is null). This tool requires the Unreal Editor."));
			}

			// Build the standard deferred-success payload.
			auto PendingResult = [&Action](const TCHAR* Note) -> FUnrealOpenMcpToolDispatchResult
			{
				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("action"), Action);
				Result->SetBoolField(TEXT("pending"), true);
				Result->SetStringField(TEXT("note"), Note);
				return FUnrealOpenMcpToolDispatchResult::Ok(
					WriteJson(MakeShared<FJsonValueObject>(Result)));
			};

			// Build an immediate (non-deferred) success payload with the observed
			// paused state (pause/resume take effect this frame).
			auto ImmediateResult = [&Action](const TCHAR* Note, bool bPaused) -> FUnrealOpenMcpToolDispatchResult
			{
				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("action"), Action);
				Result->SetBoolField(TEXT("pending"), false);
				Result->SetBoolField(TEXT("isPaused"), bPaused);
				Result->SetStringField(TEXT("note"), Note);
				return FUnrealOpenMcpToolDispatchResult::Ok(
					WriteJson(MakeShared<FJsonValueObject>(Result)));
			};

			if (Action == TEXT("start"))
			{
				if (IsPlaySessionActive())
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_transition"),
						TEXT("A play session is already active; refusing to start a second one (no silent restart). Stop first, then poll get-state."));
				}
				// RequestPlaySession only QUEUES the request; PIE actually starts
				// on a later editor tick. In-process PlayInEditor session (v1
				// scope). The agent polls get-state to observe isPlaying:true.
				FRequestPlaySessionParams SessionParams;
				SessionParams.WorldType = EPlaySessionWorldType::PlayInEditor;
				GEditor->RequestPlaySession(SessionParams);
				return PendingResult(
					TEXT("PIE start requested; the session begins on a later editor tick. Poll editor_application_get_state until isPlaying:true."));
			}

			if (Action == TEXT("stop"))
			{
				if (!IsPlaySessionActive())
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_transition"),
						TEXT("No play session is active; nothing to stop."));
				}
				// RequestEndPlayMap queues the teardown for a later tick.
				GEditor->RequestEndPlayMap();
				return PendingResult(
					TEXT("PIE stop requested; the session ends on a later editor tick. Poll editor_application_get_state until isPlaying:false."));
			}

			if (Action == TEXT("pause"))
			{
				if (!IsPlaySessionActive())
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_transition"),
						TEXT("No play session is active; cannot pause."));
				}
				if (IsPaused())
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_transition"),
						TEXT("The play session is already paused."));
				}
				APlayerController* PC = GEditor->PlayWorld->GetFirstPlayerController();
				if (PC == nullptr)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_transition"),
						TEXT("No player controller in the play session; cannot pause."));
				}
				PC->SetPause(true);
				return ImmediateResult(
					TEXT("PIE paused."), IsPaused());
			}

			if (Action == TEXT("resume"))
			{
				if (!IsPlaySessionActive())
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_transition"),
						TEXT("No play session is active; cannot resume."));
				}
				if (!IsPaused())
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_transition"),
						TEXT("The play session is not paused; nothing to resume."));
				}
				APlayerController* PC = GEditor->PlayWorld->GetFirstPlayerController();
				if (PC == nullptr)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_transition"),
						TEXT("No player controller in the play session; cannot resume."));
				}
				PC->SetPause(false);
				return ImmediateResult(
					TEXT("PIE resumed."), IsPaused());
			}

			return FUnrealOpenMcpToolDispatchResult::Fail(
				TEXT("invalid_parameter"),
				FString::Printf(
					TEXT("'action' '%s' is not one of start | stop | pause | resume."),
					*Action));
		}, FUnrealOpenMcpToolMetadata::Mutating());

	// =========================================================================
	// unreal_open_mcp_editor_selection_get — read the editor actor selection.
	// =========================================================================
	//
	// Returns { count, actors: [{ name, label, class, path }, ...] } — the same
	// identity shape the actor family emits, so a selection read chains straight
	// into actor_modify / screenshot / inspect. Read-only — no gate. Structured
	// error: editor_unavailable (no GEditor).
	Registry.Register(
		TEXT("unreal_open_mcp_editor_selection_get"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			if (GEditor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("editor_unavailable"),
					TEXT("No editor is available (GEditor is null). This tool requires the Unreal Editor."));
			}

			USelection* Selection = GEditor->GetSelectedActors();
			TArray<TSharedPtr<FJsonValue>> Actors;
			if (Selection != nullptr)
			{
				for (FSelectionIterator It(*Selection); It; ++It)
				{
					AActor* Actor = Cast<AActor>(*It);
					if (Actor != nullptr)
					{
						Actors.Add(MakeShared<FJsonValueObject>(ToActorIdentity(Actor)));
					}
				}
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("count"), Actors.Num());
			Result->SetArrayField(TEXT("actors"), Actors);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	// =========================================================================
	// unreal_open_mcp_editor_selection_set — replace / clear the selection.
	// =========================================================================
	//
	// `actors` (string[] of refs) OR `clear:true`.
	//   - clear:true  — deselect all (SelectNone). Ignores `actors`.
	//   - actors[]    — resolve EVERY ref up front (label → name → path); a
	//                   single bad ref aborts with actor_not_found BEFORE any
	//                   selection change (resolve-before-mutate), then SelectNone
	//                   + SelectActor each resolved actor.
	//   - neither     — refused with missing_parameter (an empty call must never
	//                   silently wipe the selection).
	//
	// Wrapped in FScopedTransaction (single undo) and commits with one
	// NoteSelectionChange. Mutating: the gate's mandatory `paths_hint` (level
	// package and/or selected actor paths) is enforced by the dispatcher BEFORE
	// the handler runs. Structured errors:
	//   - invalid_parameter — malformed body / `actors` not a string array
	//   - missing_parameter — neither `clear:true` nor a non-empty `actors`
	//   - actor_not_found   — a ref did not resolve (selection unchanged)
	//   - no_editor_world   — no editor world to resolve refs against
	//   - editor_unavailable— no GEditor
	Registry.Register(
		TEXT("unreal_open_mcp_editor_selection_set"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			if (GEditor == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("editor_unavailable"),
					TEXT("No editor is available (GEditor is null). This tool requires the Unreal Editor."));
			}

			const bool bClear = Args->HasTypedField<EJson::Boolean>(TEXT("clear"))
				&& Args->GetBoolField(TEXT("clear"));

			// Collect the requested refs (when not clearing). A present-but-wrong
			// -typed `actors` is a structured invalid_parameter, not a silent
			// empty list, so a caller that fat-fingers the shape learns why.
			TArray<FString> ActorRefs;
			if (!bClear && Args->HasField(TEXT("actors")))
			{
				if (!Args->HasTypedField<EJson::Array>(TEXT("actors")))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						TEXT("'actors' must be an array of actor ref strings."));
				}
				const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
				Args->TryGetArrayField(TEXT("actors"), Arr);
				if (Arr != nullptr)
				{
					for (const TSharedPtr<FJsonValue>& V : *Arr)
					{
						if (V.IsValid() && V->Type == EJson::String)
						{
							const FString Ref = V->AsString();
							if (!Ref.IsEmpty())
							{
								ActorRefs.Add(Ref);
							}
						}
					}
				}
			}

			// Empty call without clear → refuse (never a silent deselect).
			if (!bClear && ActorRefs.Num() == 0)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("Provide a non-empty 'actors' array to select, or set 'clear':true to deselect all. An empty call is refused so the selection is never silently wiped."));
			}

			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("no_editor_world"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			// Resolve EVERY ref BEFORE mutating. A single miss aborts the whole
			// call with the selection untouched (no half-applied selection).
			TArray<AActor*> Resolved;
			Resolved.Reserve(ActorRefs.Num());
			for (const FString& Ref : ActorRefs)
			{
				AActor* Actor = FUnrealOpenMcpObjectRef::ResolveActor(Ref, World);
				if (Actor == nullptr)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("actor_not_found"),
						FString::Printf(
							TEXT("actor ref '%s' did not resolve; selection unchanged."),
							*Ref));
				}
				Resolved.Add(Actor);
			}

			// Apply as one undoable transaction. SelectNone(false, true) clears
			// without emitting a per-actor change; the final NoteSelectionChange
			// fires the single selection-changed notification.
			FScopedTransaction Transaction(
				LOCTEXT("EditorSelectionSet", "Set Editor Selection"));
			GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
			for (AActor* Actor : Resolved)
			{
				GEditor->SelectActor(
					Actor, /*bInSelected*/ true, /*bNotify*/ false);
			}
			GEditor->NoteSelectionChange();

			// Build the resulting selection payload (same shape as get).
			TArray<TSharedPtr<FJsonValue>> Actors;
			for (AActor* Actor : Resolved)
			{
				Actors.Add(MakeShared<FJsonValueObject>(ToActorIdentity(Actor)));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("cleared"), bClear);
			Result->SetNumberField(TEXT("count"), Actors.Num());
			Result->SetArrayField(TEXT("actors"), Actors);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());
}

#undef LOCTEXT_NAMESPACE
