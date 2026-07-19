// unreal_open_mcp_console_get_logs / _clear_logs / _run_command Automation
// specs (P5.3), plus the FUnrealOpenMcpLogCollector unit contract.
//
// The collector is a process-wide singleton the editor module registers with
// GLog. To stay deterministic despite concurrent editor logging, every case
// injects entries under a UNIQUE test category and filters reads by that
// category — so other threads' log lines never perturb the assertions (ring
// overflow within a sub-millisecond test window is not a realistic concern).
//
// Cases mirror the P5.3 plan's test table: verbosity parse; ring bounded by
// cap; filter (verbosity / category / contains) + limit + truncated; clear
// returns the removed count; run-command missing-arg + a benign query round
// trip.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Tools/UnrealOpenMcpConsoleTools.h"
#include "Tools/UnrealOpenMcpLogCollector.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpConsoleToolsSpec,
	"UnrealOpenMcp.Tools.Console",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpConsoleToolsSpec)

namespace
{
	/** A category unique to this spec so reads isolate our injected entries. */
	const FName ConsoleSpecCategory(TEXT("LogUnrealOpenMcpConsoleSpec"));

	TSharedPtr<FJsonObject> ParseJson_Console(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	bool GetConsoleHandler(const FString& ToolName, FUnrealOpenMcpToolHandler& OutHandler)
	{
		FUnrealOpenMcpToolRegistry Registry;
		FUnrealOpenMcpConsoleTools::Register(Registry);
		return Registry.TryGet(ToolName, OutHandler);
	}

	/** Inject a line into the shared collector under the spec category. */
	void Inject(const TCHAR* Message, ELogVerbosity::Type Verbosity)
	{
		FUnrealOpenMcpLogCollector::Get().Serialize(Message, Verbosity, ConsoleSpecCategory);
	}

	/** Body that reads only the spec category with a large limit. */
	FString CategoryBody(int32 Limit = 500)
	{
		return FString::Printf(
			TEXT("{\"category\":\"%s\",\"limit\":%d}"),
			*ConsoleSpecCategory.ToString(), Limit);
	}
}

void FUnrealOpenMcpConsoleToolsSpec::Define()
{
	Describe("FUnrealOpenMcpLogCollector — unit contract", [this]()
	{
		It("parses verbosity tokens and round-trips the string form", [this]()
		{
			TestEqual(TEXT("error"),
				FUnrealOpenMcpLogCollector::ParseVerbosity(TEXT("error")), ELogVerbosity::Error);
			TestEqual(TEXT("warning (case-insensitive)"),
				FUnrealOpenMcpLogCollector::ParseVerbosity(TEXT("WARNING")), ELogVerbosity::Warning);
			TestEqual(TEXT("all (empty)"),
				FUnrealOpenMcpLogCollector::ParseVerbosity(TEXT("")), ELogVerbosity::All);
			TestEqual(TEXT("unknown → sentinel"),
				FUnrealOpenMcpLogCollector::ParseVerbosity(TEXT("bogus")), ELogVerbosity::NumVerbosity);
			TestEqual(TEXT("string form"),
				FUnrealOpenMcpLogCollector::VerbosityToString(ELogVerbosity::Warning), FString(TEXT("Warning")));
		});

		It("never exceeds the ring capacity", [this]()
		{
			FUnrealOpenMcpLogCollector::Get().Start();
			// Push more than the cap; Num must clamp at the capacity.
			for (int32 i = 0; i < FUnrealOpenMcpLogCollector::DefaultMaxEntries + 200; ++i)
			{
				Inject(TEXT("overflow"), ELogVerbosity::Log);
			}
			TestTrue(TEXT("Num <= cap"),
				FUnrealOpenMcpLogCollector::Get().Num() <= FUnrealOpenMcpLogCollector::DefaultMaxEntries);
		});
	});

	Describe("unreal_open_mcp_console_get_logs — handler contract", [this]()
	{
		It("returns injected entries filtered by category with stable fields", [this]()
		{
			FUnrealOpenMcpLogCollector::Get().Start();
			FUnrealOpenMcpLogCollector::Get().Clear();
			Inject(TEXT("alpha one"), ELogVerbosity::Warning);
			Inject(TEXT("alpha two"), ELogVerbosity::Log);

			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetConsoleHandler(TEXT("unreal_open_mcp_console_get_logs"), Handler));

			const FUnrealOpenMcpToolDispatchResult Result = Handler(CategoryBody());
			TestTrue(TEXT("ok"), Result.bOk);
			const TSharedPtr<FJsonObject> Json = ParseJson_Console(Result.Output);
			if (!TestNotNull(TEXT("json"), Json.Get()))
			{
				return;
			}
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			Json->TryGetArrayField(TEXT("entries"), Entries);
			if (!TestNotNull(TEXT("entries array"), Entries) || Entries == nullptr)
			{
				return;
			}
			TestEqual(TEXT("exactly the two injected"), Entries->Num(), 2);
			const TSharedPtr<FJsonObject>* First = nullptr;
			(*Entries)[0]->TryGetObject(First);
			if (TestNotNull(TEXT("first entry"), First ? First->Get() : nullptr) && First)
			{
				TestTrue(TEXT("sequence"), (*First)->HasField(TEXT("sequence")));
				TestTrue(TEXT("verbosity"), (*First)->HasField(TEXT("verbosity")));
				TestTrue(TEXT("category"), (*First)->HasField(TEXT("category")));
				TestTrue(TEXT("message"), (*First)->HasField(TEXT("message")));
				TestTrue(TEXT("timestamp"), (*First)->HasField(TEXT("timestamp")));
			}
		});

		It("applies the min-severity verbosity filter", [this]()
		{
			FUnrealOpenMcpLogCollector::Get().Start();
			FUnrealOpenMcpLogCollector::Get().Clear();
			Inject(TEXT("warn line"), ELogVerbosity::Warning);
			Inject(TEXT("log line"), ELogVerbosity::Log);

			FUnrealOpenMcpToolHandler Handler;
			GetConsoleHandler(TEXT("unreal_open_mcp_console_get_logs"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"category\":\"%s\",\"verbosity\":\"warning\",\"limit\":500}"),
				*ConsoleSpecCategory.ToString());
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);
			const TSharedPtr<FJsonObject> Json = ParseJson_Console(Result.Output);
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			Json->TryGetArrayField(TEXT("entries"), Entries);
			if (!TestNotNull(TEXT("entries"), Entries) || Entries == nullptr)
			{
				return;
			}
			// Only the Warning entry passes a min-severity of Warning.
			TestEqual(TEXT("only warning-level"), Entries->Num(), 1);
			const TSharedPtr<FJsonObject>* Only = nullptr;
			(*Entries)[0]->TryGetObject(Only);
			if (Only)
			{
				TestEqual(TEXT("verbosity Warning"),
					(*Only)->GetStringField(TEXT("verbosity")), FString(TEXT("Warning")));
			}
		});

		It("bounds by limit and reports matched + truncated", [this]()
		{
			FUnrealOpenMcpLogCollector::Get().Start();
			FUnrealOpenMcpLogCollector::Get().Clear();
			for (int32 i = 0; i < 5; ++i)
			{
				Inject(TEXT("limited"), ELogVerbosity::Log);
			}

			FUnrealOpenMcpToolHandler Handler;
			GetConsoleHandler(TEXT("unreal_open_mcp_console_get_logs"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"category\":\"%s\",\"limit\":2}"), *ConsoleSpecCategory.ToString());
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);
			const TSharedPtr<FJsonObject> Json = ParseJson_Console(Result.Output);
			if (!TestNotNull(TEXT("json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("count == limit"), static_cast<int32>(Json->GetNumberField(TEXT("count"))), 2);
			TestEqual(TEXT("matched == 5"), static_cast<int32>(Json->GetNumberField(TEXT("matched"))), 5);
			TestTrue(TEXT("truncated"), Json->GetBoolField(TEXT("truncated")));
		});

		It("rejects an unknown verbosity token", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetConsoleHandler(TEXT("unreal_open_mcp_console_get_logs"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{\"verbosity\":\"bogus\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});
	});

	Describe("unreal_open_mcp_console_clear_logs — handler contract", [this]()
	{
		It("empties the buffer and reports the removed count", [this]()
		{
			FUnrealOpenMcpLogCollector::Get().Start();
			FUnrealOpenMcpLogCollector::Get().Clear();
			Inject(TEXT("to clear"), ELogVerbosity::Log);

			FUnrealOpenMcpToolHandler ClearHandler;
			TestTrue(TEXT("clear handler registered"),
				GetConsoleHandler(TEXT("unreal_open_mcp_console_clear_logs"), ClearHandler));
			const FUnrealOpenMcpToolDispatchResult ClearResult = ClearHandler(TEXT("{}"));
			TestTrue(TEXT("ok"), ClearResult.bOk);
			const TSharedPtr<FJsonObject> ClearJson = ParseJson_Console(ClearResult.Output);
			if (TestNotNull(TEXT("clear json"), ClearJson.Get()))
			{
				TestTrue(TEXT("removed >= 1"),
					static_cast<int32>(ClearJson->GetNumberField(TEXT("removed"))) >= 1);
			}

			// After a clear the spec-category read is empty.
			FUnrealOpenMcpToolHandler GetHandler;
			GetConsoleHandler(TEXT("unreal_open_mcp_console_get_logs"), GetHandler);
			const FUnrealOpenMcpToolDispatchResult GetResult = GetHandler(CategoryBody());
			const TSharedPtr<FJsonObject> GetJson = ParseJson_Console(GetResult.Output);
			if (TestNotNull(TEXT("get json"), GetJson.Get()))
			{
				TestEqual(TEXT("count 0 after clear"),
					static_cast<int32>(GetJson->GetNumberField(TEXT("count"))), 0);
			}
		});
	});

	Describe("unreal_open_mcp_console_run_command — handler contract", [this]()
	{
		It("returns missing_parameter when command is absent", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetConsoleHandler(TEXT("unreal_open_mcp_console_run_command"), Handler));
			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});

		It("runs a benign cvar query and returns command/output/handled", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetConsoleHandler(TEXT("unreal_open_mcp_console_run_command"), Handler);
			// Querying a cvar prints its value and is handled; it changes no state.
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"command\":\"r.ScreenPercentage\"}"));
			TestTrue(TEXT("ok"), Result.bOk);
			const TSharedPtr<FJsonObject> Json = ParseJson_Console(Result.Output);
			if (!TestNotNull(TEXT("json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("command echoed"),
				Json->GetStringField(TEXT("command")), FString(TEXT("r.ScreenPercentage")));
			TestTrue(TEXT("has output"), Json->HasField(TEXT("output")));
			TestTrue(TEXT("has handled"), Json->HasField(TEXT("handled")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
