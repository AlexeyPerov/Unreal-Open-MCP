// unreal_open_mcp_reflection_method_find / _method_call Automation specs
// (P5.4).
//
// Pins the reflection pair at the handler level. Cases mirror the P5.4 plan's
// test table: find shape + honesty (matched/returned) + missing/bad class;
// call target-XOR-class validation; method_not_found; the safety gate
// (method_not_callable on the CDO path for an instance method); and a happy
// invoke of a BlueprintCallable pure getter on a spawned actor.
//
// The safety + happy cases are guarded by a find probe so they stay robust if
// a specific engine UFunction name is absent in a given engine version (the
// case is skipped-with-pass rather than failing on a name drift).
//
// Cleanup: the one spawned actor is destroyed via the FActorScope RAII helper.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Tools/UnrealOpenMcpReflectionTools.h"
#include "Tools/UnrealOpenMcpObjectRef.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "EngineUtils.h"            // TActorIterator
#include "Engine/World.h"
#include "GameFramework/Actor.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpReflectionSpec,
	"UnrealOpenMcp.Tools.Reflection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpReflectionSpec)

namespace
{
	constexpr const TCHAR* ReflTestActorPrefix = TEXT("UnrealOpenMcpTestActor_Reflection_");

	TSharedPtr<FJsonObject> ParseJson_Reflection(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	bool GetReflectionHandler(const FString& ToolName, FUnrealOpenMcpToolHandler& OutHandler)
	{
		FUnrealOpenMcpToolRegistry Registry;
		FUnrealOpenMcpReflectionTools::Register(Registry);
		return Registry.TryGet(ToolName, OutHandler);
	}

	/** True when find(class) returns a method named exactly @p MethodName. Used
	 *  to guard engine-name-dependent cases against version drift. */
	bool FindHasMethod(const FString& ClassRef, const FString& MethodName)
	{
		FUnrealOpenMcpToolHandler Handler;
		if (!GetReflectionHandler(TEXT("unreal_open_mcp_reflection_method_find"), Handler))
		{
			return false;
		}
		const FString Body = FString::Printf(TEXT("{\"class\":\"%s\",\"limit\":0}"), *ClassRef);
		const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
		if (!Result.bOk)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Json = ParseJson_Reflection(Result.Output);
		if (!Json.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Methods = nullptr;
		if (!Json->TryGetArrayField(TEXT("methods"), Methods) || Methods == nullptr)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& V : *Methods)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (V->TryGetObject(Obj) && Obj && (*Obj)->GetStringField(TEXT("name")) == MethodName)
			{
				return true;
			}
		}
		return false;
	}

	struct FReflectionActorScope
	{
		TArray<AActor*> Spawned;
		UWorld* World = nullptr;
		~FReflectionActorScope()
		{
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

void FUnrealOpenMcpReflectionSpec::Define()
{
	Describe("unreal_open_mcp_reflection_method_find — handler contract", [this]()
	{
		It("returns a method roster with matched/returned + descriptors", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetReflectionHandler(TEXT("unreal_open_mcp_reflection_method_find"), Handler));

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"class\":\"/Script/Engine.Actor\"}"));
			TestTrue(TEXT("ok"), Result.bOk);
			const TSharedPtr<FJsonObject> Json = ParseJson_Reflection(Result.Output);
			if (!TestNotNull(TEXT("json"), Json.Get()))
			{
				return;
			}
			TestTrue(TEXT("has class"), Json->HasField(TEXT("class")));
			TestTrue(TEXT("has matched"), Json->HasTypedField<EJson::Number>(TEXT("matched")));
			TestTrue(TEXT("has returned"), Json->HasTypedField<EJson::Number>(TEXT("returned")));
			TestTrue(TEXT("matched > 0"),
				static_cast<int32>(Json->GetNumberField(TEXT("matched"))) > 0);
			const TArray<TSharedPtr<FJsonValue>>* Methods = nullptr;
			Json->TryGetArrayField(TEXT("methods"), Methods);
			if (!TestNotNull(TEXT("methods array"), Methods) || Methods == nullptr || Methods->Num() == 0)
			{
				return;
			}
			const TSharedPtr<FJsonObject>* First = nullptr;
			(*Methods)[0]->TryGetObject(First);
			if (First)
			{
				TestTrue(TEXT("name"), (*First)->HasField(TEXT("name")));
				TestTrue(TEXT("params"), (*First)->HasField(TEXT("params")));
				TestTrue(TEXT("returnType"), (*First)->HasField(TEXT("returnType")));
				TestTrue(TEXT("flags"), (*First)->HasField(TEXT("flags")));
				TestTrue(TEXT("callable"), (*First)->HasField(TEXT("callable")));
			}
		});

		It("returns missing_parameter when class is absent", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetReflectionHandler(TEXT("unreal_open_mcp_reflection_method_find"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});

		It("returns class_not_found for an unresolvable class", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetReflectionHandler(TEXT("unreal_open_mcp_reflection_method_find"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"class\":\"NoSuchClass_ZZZ\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("class_not_found")));
		});
	});

	Describe("unreal_open_mcp_reflection_method_call — handler contract", [this]()
	{
		It("rejects both target and class with ambiguous_target", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetReflectionHandler(TEXT("unreal_open_mcp_reflection_method_call"), Handler));
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"method\":\"Foo\",\"target\":\"X\",\"class\":\"/Script/Engine.Actor\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("ambiguous_target")));
		});

		It("rejects neither target nor class with missing_parameter", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetReflectionHandler(TEXT("unreal_open_mcp_reflection_method_call"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{\"method\":\"Foo\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});

		It("returns method_not_found for an unknown method on the CDO", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetReflectionHandler(TEXT("unreal_open_mcp_reflection_method_call"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"method\":\"NoSuchFunction_ZZZ\",\"class\":\"/Script/Engine.Actor\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("method_not_found")));
		});

		It("rejects an instance method invoked on the CDO with method_not_callable", [this]()
		{
			// K2_DestroyActor is BlueprintCallable but not static/CallInEditor —
			// calling it on the CDO must be refused. Guarded against name drift.
			if (!FindHasMethod(TEXT("/Script/Engine.Actor"), TEXT("K2_DestroyActor")))
			{
				return;
			}
			FUnrealOpenMcpToolHandler Handler;
			GetReflectionHandler(TEXT("unreal_open_mcp_reflection_method_call"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"method\":\"K2_DestroyActor\",\"class\":\"/Script/Engine.Actor\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("method_not_callable")));
		});

		It("invokes a BlueprintCallable getter on an instance and returns a value", [this]()
		{
			// K2_GetActorLocation is BlueprintCallable + pure with no inputs and
			// an FVector return. Guarded against name drift.
			if (!FindHasMethod(TEXT("/Script/Engine.Actor"), TEXT("K2_GetActorLocation")))
			{
				return;
			}
			FReflectionActorScope Scope;
			if (!TestNotNull(TEXT("editor world"), Scope.World = FUnrealOpenMcpObjectRef::GetEditorWorld()))
			{
				return;
			}
			AActor* A = Scope.Spawn(FString(ReflTestActorPrefix) + TEXT("Getter"));
			if (!TestNotNull(TEXT("spawned actor"), A))
			{
				return;
			}

			FUnrealOpenMcpToolHandler Handler;
			GetReflectionHandler(TEXT("unreal_open_mcp_reflection_method_call"), Handler);
			const FString Body = FString::Printf(
				TEXT("{\"method\":\"K2_GetActorLocation\",\"target\":\"%s\"}"),
				*A->GetActorLabel());
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);
			const TSharedPtr<FJsonObject> Json = ParseJson_Reflection(Result.Output);
			if (!TestNotNull(TEXT("json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("method echoed"),
				Json->GetStringField(TEXT("method")), FString(TEXT("K2_GetActorLocation")));
			TestTrue(TEXT("has target"), Json->HasField(TEXT("target")));
			TestTrue(TEXT("returnValue present"), Json->HasField(TEXT("returnValue")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
