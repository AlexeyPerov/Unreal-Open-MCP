// unreal_open_mcp_editor_selection_get / _set Automation specs (P5.2).
//
// Pins the editor selection pair at the handler level against the live editor
// USelection. The cases mirror the P5.2 plan's test table:
//   - get: returns { count, actors:[{ name, label, class, path }] } — the P2
//     identity shape.
//   - set with refs: selects exactly those actors (count matches; the live
//     GEditor selection reflects them).
//   - set with clear:true: deselects all → count 0.
//   - empty call without clear: refused with missing_parameter, selection
//     unchanged.
//   - bad ref: aborts with actor_not_found BEFORE mutating — a prior selection
//     is left intact (resolve-before-mutate).
//   - multi-select: two refs → count 2.
//
// Cleanup: every spawned actor is destroyed at the end of the case via the
// FActorScope RAII helper (copied from the level specs) with a stray sweep, and
// the selection is cleared so a leaked selection does not bleed into later
// cases.
//
// Adapted from Unity's selection-get / selection-set tests: actor refs (label →
// name → path) replace Unity's instance_id, and the explicit-clear-required +
// resolve-before-mutate rules copy Unreal-MCP's selection contract.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Tools/UnrealOpenMcpEditorTools.h"
#include "Tools/UnrealOpenMcpObjectRef.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "EngineUtils.h"            // TActorIterator
#include "Engine/World.h"
#include "Engine/Selection.h"       // USelection
#include "GameFramework/Actor.h"
#include "Editor.h"                 // GEditor

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpEditorSelectionSpec,
	"UnrealOpenMcp.Tools.EditorSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpEditorSelectionSpec)

namespace
{
	/** Prefix for every test actor so clean-up finds exactly what we spawned. */
	constexpr const TCHAR* SelTestActorPrefix = TEXT("UnrealOpenMcpTestActor_Selection_");

	/** Parse a JSON object from a string. Null on failure. */
	TSharedPtr<FJsonObject> ParseJson_Selection(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	/** Resolve an editor tool handler by name from a fresh registry. */
	bool GetSelectionHandler(const FString& ToolName, FUnrealOpenMcpToolHandler& OutHandler)
	{
		FUnrealOpenMcpToolRegistry Registry;
		FUnrealOpenMcpEditorTools::Register(Registry);
		return Registry.TryGet(ToolName, OutHandler);
	}

	/** Count the currently-selected actors on the live editor selection. */
	int32 SelectedActorCount()
	{
		if (GEditor == nullptr)
		{
			return 0;
		}
		USelection* Selection = GEditor->GetSelectedActors();
		if (Selection == nullptr)
		{
			return 0;
		}
		int32 Count = 0;
		for (FSelectionIterator It(*Selection); It; ++It)
		{
			if (Cast<AActor>(*It) != nullptr)
			{
				++Count;
			}
		}
		return Count;
	}

	/** True when @p Actor is currently selected in the editor. */
	bool IsActorSelected(AActor* Actor)
	{
		return GEditor != nullptr && Actor != nullptr && GEditor->GetSelectedActors()
			&& GEditor->GetSelectedActors()->IsSelected(Actor);
	}

	/**
	 * RAII spawn-tracking helper, copied from the level specs. Destroys every
	 * tracked actor + any test-prefixed strays and clears the editor selection
	 * on teardown so a leaked selection never bleeds into a later case.
	 */
	struct FSelectionActorScope
	{
		TArray<AActor*> Spawned;
		UWorld* World = nullptr;

		~FSelectionActorScope()
		{
			if (GEditor != nullptr)
			{
				GEditor->SelectNone(/*bNoteSelectionChange*/ true, /*bDeselectBSPSurfs*/ true);
			}
			if (World == nullptr)
			{
				World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			}
			for (AActor* Actor : Spawned)
			{
				if (Actor != nullptr && IsValid(Actor))
				{
					World->DestroyActor(Actor, true);
				}
			}
			if (World != nullptr)
			{
				TArray<AActor*> Strays;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (Actor != nullptr
						&& Actor->GetActorLabel().StartsWith(SelTestActorPrefix, ESearchCase::CaseSensitive))
					{
						Strays.Add(Actor);
					}
				}
				for (AActor* Actor : Strays)
				{
					World->DestroyActor(Actor, true);
				}
			}
		}

		AActor* Spawn(const FString& Label)
		{
			if (World == nullptr)
			{
				World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			}
			if (World == nullptr)
			{
				return nullptr;
			}
			AActor* Actor = World->SpawnActor<AActor>();
			if (Actor != nullptr)
			{
				Actor->SetActorLabel(*Label);
				Spawned.Add(Actor);
			}
			return Actor;
		}
	};
}

void FUnrealOpenMcpEditorSelectionSpec::Define()
{
	Describe("unreal_open_mcp_editor_selection_get — handler contract", [this]()
	{
		// Returns { count, actors:[...] } — the P2 identity shape. After a clear
		// + a single select, get reflects exactly that one actor.
		It("returns the selected actors as identity refs", [this]()
		{
			FSelectionActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			AActor* A = Scope.Spawn(FString(SelTestActorPrefix) + TEXT("GetA"));
			if (!TestNotNull(TEXT("spawned actor"), A))
			{
				return;
			}
			GEditor->SelectNone(true, true);
			GEditor->SelectActor(A, true, true);

			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetSelectionHandler(TEXT("unreal_open_mcp_editor_selection_get"), Handler));

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson_Selection(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("count == 1"), static_cast<int32>(Json->GetNumberField(TEXT("count"))), 1);

			const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
			Json->TryGetArrayField(TEXT("actors"), Actors);
			if (!TestNotNull(TEXT("actors array"), Actors) || Actors == nullptr || Actors->Num() == 0)
			{
				return;
			}
			const TSharedPtr<FJsonObject>* First = nullptr;
			(*Actors)[0]->TryGetObject(First);
			if (!TestNotNull(TEXT("first entry"), First) || First == nullptr || !First->IsValid())
			{
				return;
			}
			// Identity-ref field set: name / label / class / path.
			TestTrue(TEXT("name field"), (*First)->HasField(TEXT("name")));
			TestTrue(TEXT("label field"), (*First)->HasField(TEXT("label")));
			TestTrue(TEXT("class field"), (*First)->HasField(TEXT("class")));
			TestTrue(TEXT("path field"), (*First)->HasField(TEXT("path")));
		});
	});

	Describe("unreal_open_mcp_editor_selection_set — handler contract", [this]()
	{
		// Set with refs selects exactly those actors; the live selection reflects
		// them.
		It("selects exactly the referenced actors", [this]()
		{
			FSelectionActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			AActor* A = Scope.Spawn(FString(SelTestActorPrefix) + TEXT("SetA"));
			AActor* B = Scope.Spawn(FString(SelTestActorPrefix) + TEXT("SetB"));
			if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B))
			{
				return;
			}
			GEditor->SelectNone(true, true);

			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetSelectionHandler(TEXT("unreal_open_mcp_editor_selection_set"), Handler));

			const FString Body = FString::Printf(
				TEXT("{\"actors\":[\"%s\",\"%s\"]}"),
				*A->GetActorLabel(), *B->GetActorLabel());
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson_Selection(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("count == 2"), static_cast<int32>(Json->GetNumberField(TEXT("count"))), 2);
			// The live editor selection reflects exactly A and B.
			TestEqual(TEXT("live selection count 2"), SelectedActorCount(), 2);
			TestTrue(TEXT("A selected"), IsActorSelected(A));
			TestTrue(TEXT("B selected"), IsActorSelected(B));
		});

		// clear:true deselects all → count 0.
		It("clears the selection with clear:true", [this]()
		{
			FSelectionActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			AActor* A = Scope.Spawn(FString(SelTestActorPrefix) + TEXT("ClearA"));
			GEditor->SelectNone(true, true);
			GEditor->SelectActor(A, true, true);
			TestEqual(TEXT("precondition: 1 selected"), SelectedActorCount(), 1);

			FUnrealOpenMcpToolHandler Handler;
			GetSelectionHandler(TEXT("unreal_open_mcp_editor_selection_set"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{\"clear\":true}"));
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson_Selection(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestTrue(TEXT("cleared true"), Json->GetBoolField(TEXT("cleared")));
			TestEqual(TEXT("count 0"), static_cast<int32>(Json->GetNumberField(TEXT("count"))), 0);
			TestEqual(TEXT("live selection empty"), SelectedActorCount(), 0);
		});

		// Empty call without clear → missing_parameter, selection unchanged.
		It("refuses an empty call and leaves the selection unchanged", [this]()
		{
			FSelectionActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			AActor* A = Scope.Spawn(FString(SelTestActorPrefix) + TEXT("EmptyA"));
			GEditor->SelectNone(true, true);
			GEditor->SelectActor(A, true, true);

			FUnrealOpenMcpToolHandler Handler;
			GetSelectionHandler(TEXT("unreal_open_mcp_editor_selection_set"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
			// Selection untouched — A is still selected.
			TestEqual(TEXT("selection unchanged (1)"), SelectedActorCount(), 1);
			TestTrue(TEXT("A still selected"), IsActorSelected(A));
		});

		// Bad ref aborts BEFORE mutating — a prior selection is left intact.
		It("aborts on a bad ref and leaves the prior selection intact", [this]()
		{
			FSelectionActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			AActor* A = Scope.Spawn(FString(SelTestActorPrefix) + TEXT("PriorA"));
			GEditor->SelectNone(true, true);
			GEditor->SelectActor(A, true, true);
			TestEqual(TEXT("precondition: 1 selected"), SelectedActorCount(), 1);

			FUnrealOpenMcpToolHandler Handler;
			GetSelectionHandler(TEXT("unreal_open_mcp_editor_selection_set"), Handler);

			// One good ref + one nonexistent ref. Resolve-before-mutate means the
			// whole call aborts and the prior selection (A) is untouched.
			const FString Body = FString::Printf(
				TEXT("{\"actors\":[\"%s\",\"UnrealOpenMcp_NoSuchActor_ZZZ\"]}"),
				*A->GetActorLabel());
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("actor_not_found")));
			// Prior selection intact — A still selected, count unchanged.
			TestEqual(TEXT("prior selection intact (1)"), SelectedActorCount(), 1);
			TestTrue(TEXT("A still selected"), IsActorSelected(A));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
