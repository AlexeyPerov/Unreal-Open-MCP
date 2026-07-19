// Reflection tool family — see header for the safety allow-list (flags gate),
// the target XOR class (instance vs CDO) rule, and the accepted-risk note.
#include "Tools/UnrealOpenMcpReflectionTools.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Tools/UnrealOpenMcpObjectRef.h"

#include "UObject/Class.h"
#include "UObject/UnrealType.h"     // FProperty, TFieldIterator, CPF_* flags
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "JsonObjectConverter.h"

namespace
{
	/** Default cap on the number of methods returned by find. */
	constexpr int32 DefaultFindLimit = 100;

	/** Parse the raw POST body into a JSON object (empty → empty object,
	 *  malformed → null). Same contract as the other tool families. */
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

	/** Serialize a JsonValue to a compact string ("null" on null). */
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

	/** True when @p Func is CallInEditor (editor-only metadata). */
	bool IsCallInEditor(const UFunction* Func)
	{
#if WITH_EDITOR
		return Func != nullptr && Func->GetBoolMetaData(TEXT("CallInEditor"));
#else
		return false;
#endif
	}

	/**
	 * The invoke allow-list: only BlueprintCallable or CallInEditor functions
	 * may be invoked at all. Native-only / internal functions are refused so the
	 * tool is not an arbitrary-ProcessEvent surface.
	 */
	bool IsCallableForInvoke(const UFunction* Func)
	{
		if (Func == nullptr)
		{
			return false;
		}
		return Func->HasAnyFunctionFlags(FUNC_BlueprintCallable) || IsCallInEditor(Func);
	}

	/**
	 * The CDO allow-list: calling a method on a shared class-default-object is
	 * only safe for static functions or CallInEditor entry points. An instance
	 * method invoked on the CDO could corrupt the archetype, so it is refused.
	 */
	bool IsAllowedOnCDO(const UFunction* Func)
	{
		if (Func == nullptr)
		{
			return false;
		}
		return Func->HasAnyFunctionFlags(FUNC_Static) || IsCallInEditor(Func);
	}

	/** Describe one UFunction: { name, returnType, params:[{name,type,out}],
	 *  flags:[...], callable }. */
	TSharedRef<FJsonObject> DescribeFunction(UFunction* Func)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Func->GetName());

		FString ReturnType = TEXT("void");
		TArray<TSharedPtr<FJsonValue>> Params;
		for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			FProperty* Prop = *It;
			const FString TypeStr = Prop->GetCPPType();
			if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ReturnType = TypeStr;
				continue;
			}
			TSharedRef<FJsonObject> ParamJson = MakeShared<FJsonObject>();
			ParamJson->SetStringField(TEXT("name"), Prop->GetName());
			ParamJson->SetStringField(TEXT("type"), TypeStr);
			// Out (by-ref) params are surfaced back to the caller after the call.
			ParamJson->SetBoolField(
				TEXT("out"),
				Prop->HasAnyPropertyFlags(CPF_OutParm) && !Prop->HasAnyPropertyFlags(CPF_ConstParm));
			Params.Add(MakeShared<FJsonValueObject>(ParamJson));
		}
		Json->SetStringField(TEXT("returnType"), ReturnType);
		Json->SetArrayField(TEXT("params"), Params);

		TArray<TSharedPtr<FJsonValue>> Flags;
		auto AddFlag = [&Flags](const TCHAR* Name)
		{
			Flags.Add(MakeShared<FJsonValueString>(Name));
		};
		if (Func->HasAnyFunctionFlags(FUNC_BlueprintCallable)) AddFlag(TEXT("BlueprintCallable"));
		if (Func->HasAnyFunctionFlags(FUNC_BlueprintPure))     AddFlag(TEXT("BlueprintPure"));
		if (Func->HasAnyFunctionFlags(FUNC_Static))            AddFlag(TEXT("Static"));
		if (Func->HasAnyFunctionFlags(FUNC_Const))             AddFlag(TEXT("Const"));
		if (Func->HasAnyFunctionFlags(FUNC_Native))            AddFlag(TEXT("Native"));
		if (Func->HasAnyFunctionFlags(FUNC_Event))             AddFlag(TEXT("Event"));
		if (IsCallInEditor(Func))                              AddFlag(TEXT("CallInEditor"));
		Json->SetArrayField(TEXT("flags"), Flags);

		Json->SetBoolField(TEXT("callable"), IsCallableForInvoke(Func));
		return Json;
	}
}

void FUnrealOpenMcpReflectionTools::Register(FUnrealOpenMcpToolRegistry& Registry)
{
	// =========================================================================
	// unreal_open_mcp_reflection_method_find — list UFunctions on a class.
	// =========================================================================
	//
	// `class` (required) resolves via ResolveClass (native path / BP path /
	// short name). `name` is an optional case-insensitive substring; `limit`
	// bounds the result (default 100, <=0 = all). Overrides are de-duped
	// most-derived-wins. Result: { class, matched, returned, truncated,
	// methods:[...] }. Read-only. Structured errors:
	//   - invalid_parameter — malformed body
	//   - missing_parameter — `class` absent
	//   - class_not_found   — `class` did not resolve
	Registry.Register(
		TEXT("unreal_open_mcp_reflection_method_find"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString ClassRef = Args->HasTypedField<EJson::String>(TEXT("class"))
				? Args->GetStringField(TEXT("class"))
				: FString();
			if (ClassRef.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'class' is required (native class path, Blueprint path, or short type name)."));
			}

			UClass* Class = FUnrealOpenMcpObjectRef::ResolveClass(ClassRef);
			if (Class == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("class_not_found"),
					FString::Printf(TEXT("class '%s' did not resolve to a class."), *ClassRef));
			}

			const FString NameFilter = Args->HasTypedField<EJson::String>(TEXT("name"))
				? Args->GetStringField(TEXT("name"))
				: FString();

			int32 Limit = DefaultFindLimit;
			if (Args->HasTypedField<EJson::Number>(TEXT("limit")))
			{
				Limit = static_cast<int32>(Args->GetNumberField(TEXT("limit")));
			}

			int32 Matched = 0;
			TArray<TSharedPtr<FJsonValue>> Methods;
			TSet<FName> Seen;   // de-dup overrides most-derived-wins
			for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				UFunction* Func = *It;
				if (Func == nullptr)
				{
					continue;
				}
				// IncludeSuper walks derived → base, so the FIRST time a name is
				// seen is the most-derived override; later base copies are skipped.
				bool bAlreadySeen = false;
				Seen.Add(Func->GetFName(), &bAlreadySeen);
				if (bAlreadySeen)
				{
					continue;
				}
				if (!NameFilter.IsEmpty()
					&& !Func->GetName().Contains(NameFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				++Matched;
				// Materialize entries only up to the limit; keep counting matched.
				if (Limit > 0 && Methods.Num() >= Limit)
				{
					continue;
				}
				Methods.Add(MakeShared<FJsonValueObject>(DescribeFunction(Func)));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("class"), Class->GetPathName());
			Result->SetNumberField(TEXT("matched"), Matched);
			Result->SetNumberField(TEXT("returned"), Methods.Num());
			Result->SetBoolField(TEXT("truncated"), Matched > Methods.Num());
			Result->SetArrayField(TEXT("methods"), Methods);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	// =========================================================================
	// unreal_open_mcp_reflection_method_call — invoke a method (safety-gated).
	// =========================================================================
	//
	// `method` (required). `target` XOR `class` (instance vs CDO). `args` is an
	// optional object map of param-name → value (FProperty ⇄ JSON codec).
	// Result: { method, target?|class?, returnValue?, outs? }. Mutating: the
	// gate's mandatory `paths_hint` is enforced by the dispatcher BEFORE the
	// handler runs. Structured errors:
	//   - invalid_parameter  — malformed body
	//   - missing_parameter  — `method` absent / neither target nor class
	//   - ambiguous_target   — both target AND class supplied
	//   - target_not_found   — `target` ref did not resolve
	//   - class_not_found    — `class` did not resolve
	//   - method_not_found   — no such function on the class
	//   - method_not_callable— function is not BlueprintCallable / CallInEditor
	//                          (or, on the CDO path, not static / CallInEditor)
	//   - invalid_argument   — a supplied arg failed to convert to its param
	Registry.Register(
		TEXT("unreal_open_mcp_reflection_method_call"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString Method = Args->HasTypedField<EJson::String>(TEXT("method"))
				? Args->GetStringField(TEXT("method"))
				: FString();
			if (Method.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'method' is required (the UFunction name to invoke)."));
			}

			const FString TargetRef = Args->HasTypedField<EJson::String>(TEXT("target"))
				? Args->GetStringField(TEXT("target"))
				: FString();
			const FString ClassRef = Args->HasTypedField<EJson::String>(TEXT("class"))
				? Args->GetStringField(TEXT("class"))
				: FString();

			// Target XOR class: reject both, and reject neither.
			if (!TargetRef.IsEmpty() && !ClassRef.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("ambiguous_target"),
					TEXT("Supply exactly one of 'target' (an instance ref) or 'class' (the CDO), not both."));
			}
			if (TargetRef.IsEmpty() && ClassRef.IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("Supply either 'target' (an instance ref) or 'class' (the CDO)."));
			}

			// Resolve the invoke target.
			UObject* Object = nullptr;
			bool bIsCDO = false;
			if (!TargetRef.IsEmpty())
			{
				Object = FUnrealOpenMcpObjectRef::ResolveObject(TargetRef);
				if (Object == nullptr)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("target_not_found"),
						FString::Printf(TEXT("target '%s' did not resolve to an object."), *TargetRef));
				}
			}
			else
			{
				UClass* Class = FUnrealOpenMcpObjectRef::ResolveClass(ClassRef);
				if (Class == nullptr)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("class_not_found"),
						FString::Printf(TEXT("class '%s' did not resolve to a class."), *ClassRef));
				}
				Object = Class->GetDefaultObject();
				bIsCDO = true;
			}

			// Find the function on the object's class.
			UFunction* Func = Object->FindFunction(FName(*Method));
			if (Func == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("method_not_found"),
					FString::Printf(
						TEXT("method '%s' was not found on '%s'."),
						*Method, *Object->GetClass()->GetName()));
			}

			// Safety gate: allow-list by flags. On the CDO path also require the
			// function be static / CallInEditor.
			if (!IsCallableForInvoke(Func))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("method_not_callable"),
					FString::Printf(
						TEXT("method '%s' is not BlueprintCallable or CallInEditor; refusing to invoke."),
						*Method));
			}
			if (bIsCDO && !IsAllowedOnCDO(Func))
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("method_not_callable"),
					FString::Printf(
						TEXT("method '%s' is an instance method; invoking it on the class default object requires a static or CallInEditor function."),
						*Method));
			}

			// Build the parameter frame. Allocate a properly-aligned buffer and
			// initialize every param property so ProcessEvent sees valid values.
			const int32 ParmsSize = FMath::Max<int32>(Func->ParmsSize, 1);
			uint8* Parms = static_cast<uint8*>(FMemory::Malloc(ParmsSize, Func->GetMinAlignment()));
			FMemory::Memzero(Parms, ParmsSize);
			for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				It->InitializeValue_InContainer(Parms);
			}

			// Set input params from the args map. A conversion failure aborts the
			// call (after cleaning up the frame) with invalid_argument.
			const TSharedPtr<FJsonObject>* ArgMap = nullptr;
			Args->TryGetObjectField(TEXT("args"), ArgMap);
			FString ConvertError;
			for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				FProperty* Prop = *It;
				if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					continue;
				}
				if (ArgMap == nullptr || !(*ArgMap).IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonValue> Value = (*ArgMap)->TryGetField(Prop->GetName());
				if (!Value.IsValid())
				{
					continue;   // param not supplied — keep the initialized default
				}
				void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Parms);
				if (!FJsonObjectConverter::JsonValueToUProperty(Value, Prop, ValuePtr, 0, 0))
				{
					ConvertError = Prop->GetName();
					break;
				}
			}

			if (!ConvertError.IsEmpty())
			{
				for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
				{
					It->DestroyValue_InContainer(Parms);
				}
				FMemory::Free(Parms);
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_argument"),
					FString::Printf(TEXT("argument '%s' could not be converted to its parameter type."), *ConvertError));
			}

			// Invoke.
			Object->ProcessEvent(Func, Parms);

			// Read the return value + out-params back out.
			TSharedPtr<FJsonValue> ReturnValue;
			TSharedRef<FJsonObject> Outs = MakeShared<FJsonObject>();
			int32 OutCount = 0;
			for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				FProperty* Prop = *It;
				const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Parms);
				if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					ReturnValue = FJsonObjectConverter::UPropertyToJsonValue(Prop, ValuePtr, 0, 0);
				}
				else if (Prop->HasAnyPropertyFlags(CPF_OutParm) && !Prop->HasAnyPropertyFlags(CPF_ConstParm))
				{
					Outs->SetField(Prop->GetName(), FJsonObjectConverter::UPropertyToJsonValue(Prop, ValuePtr, 0, 0));
					++OutCount;
				}
			}

			// Clean up the frame (free any strings/arrays the params allocated).
			for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				It->DestroyValue_InContainer(Parms);
			}
			FMemory::Free(Parms);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("method"), Method);
			if (bIsCDO)
			{
				Result->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
			}
			else
			{
				Result->SetStringField(TEXT("target"), Object->GetPathName());
			}
			if (ReturnValue.IsValid())
			{
				Result->SetField(TEXT("returnValue"), ReturnValue);
			}
			if (OutCount > 0)
			{
				Result->SetObjectField(TEXT("outs"), Outs);
			}
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());
}
