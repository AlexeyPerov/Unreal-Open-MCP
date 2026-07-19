// Console tool family — see header for the ring-vs-call-scoped-logs
// relationship, the read-only clear classification, and the run-command
// accepted-risk note.
#include "Tools/UnrealOpenMcpConsoleTools.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Tools/UnrealOpenMcpLogCollector.h"
#include "Tools/UnrealOpenMcpObjectRef.h"

#include "Engine/Engine.h"              // GEngine->Exec
#include "Engine/World.h"
#include "Misc/OutputDevice.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	/** Default / hard-cap on the number of returned log entries. */
	constexpr int32 DefaultLogLimit = 200;
	constexpr int32 MaxLogLimit = 2000;

	/** Minimal FOutputDevice that concatenates everything Exec writes into a
	 *  string. Self-contained so the tool does not depend on the exact header
	 *  home of the engine's FStringOutputDevice (which has moved across UE
	 *  versions). */
	class FConsoleCaptureDevice : public FOutputDevice
	{
	public:
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type, const FName&) override
		{
			if (!Captured.IsEmpty())
			{
				Captured += TEXT('\n');
			}
			Captured += V;
		}
		FString Captured;
	};

	/** Parse the raw POST body into a JSON object (empty body → empty object,
	 *  malformed → null). Same contract as the actor / editor families. */
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

	/** Serialize one log entry into { sequence, verbosity, category, message,
	 *  timestamp } (ISO-8601 UTC timestamp). */
	TSharedRef<FJsonObject> EntryToJson(const FUnrealOpenMcpLogCollector::FEntry& Entry)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		// Sequence is a uint64; JSON numbers are doubles, so emit it as a number
		// (safe well past any realistic session length) plus keep it stable.
		Json->SetNumberField(TEXT("sequence"), static_cast<double>(Entry.Sequence));
		Json->SetStringField(TEXT("verbosity"), FUnrealOpenMcpLogCollector::VerbosityToString(Entry.Verbosity));
		Json->SetStringField(TEXT("category"), Entry.Category.ToString());
		Json->SetStringField(TEXT("message"), Entry.Message);
		Json->SetStringField(TEXT("timestamp"), Entry.Timestamp.ToIso8601());
		return Json;
	}
}

void FUnrealOpenMcpConsoleTools::Register(FUnrealOpenMcpToolRegistry& Registry)
{
	// =========================================================================
	// unreal_open_mcp_console_get_logs — read a filtered slice of the ring.
	// =========================================================================
	//
	// Args (all optional): `verbosity` (min severity; fatal|error|warning|
	// display|log|verbose|veryverbose|all), `category` (exact, case-insensitive),
	// `contains` (case-insensitive substring), `limit` (default 200, hard cap
	// 2000). Result: { entries:[...], count, matched, truncated }. Read-only.
	// Structured error: invalid_parameter (malformed body / unknown verbosity).
	Registry.Register(
		TEXT("unreal_open_mcp_console_get_logs"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			FUnrealOpenMcpLogCollector::FFilter Filter;

			if (Args->HasTypedField<EJson::String>(TEXT("verbosity")))
			{
				const FString Token = Args->GetStringField(TEXT("verbosity"));
				const ELogVerbosity::Type Parsed = FUnrealOpenMcpLogCollector::ParseVerbosity(Token);
				if (Parsed == ELogVerbosity::NumVerbosity)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						FString::Printf(
							TEXT("verbosity '%s' is not one of fatal|error|warning|display|log|verbose|veryverbose|all."),
							*Token));
				}
				Filter.MinVerbosity = Parsed;
			}

			if (Args->HasTypedField<EJson::String>(TEXT("category")))
			{
				const FString Cat = Args->GetStringField(TEXT("category"));
				if (!Cat.IsEmpty())
				{
					Filter.Category = FName(*Cat);
				}
			}

			if (Args->HasTypedField<EJson::String>(TEXT("contains")))
			{
				Filter.Contains = Args->GetStringField(TEXT("contains"));
			}

			int32 Limit = DefaultLogLimit;
			if (Args->HasTypedField<EJson::Number>(TEXT("limit")))
			{
				const int32 Requested = static_cast<int32>(Args->GetNumberField(TEXT("limit")));
				if (Requested > 0)
				{
					Limit = FMath::Min(Requested, MaxLogLimit);
				}
			}
			Filter.Limit = Limit;

			int32 Matched = 0;
			const TArray<FUnrealOpenMcpLogCollector::FEntry> Entries =
				FUnrealOpenMcpLogCollector::Get().Snapshot(Filter, Matched);

			TArray<TSharedPtr<FJsonValue>> EntriesJson;
			EntriesJson.Reserve(Entries.Num());
			for (const FUnrealOpenMcpLogCollector::FEntry& Entry : Entries)
			{
				EntriesJson.Add(MakeShared<FJsonValueObject>(EntryToJson(Entry)));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("entries"), EntriesJson);
			Result->SetNumberField(TEXT("count"), EntriesJson.Num());
			Result->SetNumberField(TEXT("matched"), Matched);
			Result->SetBoolField(TEXT("truncated"), Matched > EntriesJson.Num());
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	// =========================================================================
	// unreal_open_mcp_console_clear_logs — empty the ring buffer.
	// =========================================================================
	//
	// Result: { removed }. Read-only classification: the buffer is a plugin-
	// owned diagnostic ring, NOT project/editor state, so clearing it is not a
	// gated mutation (mirrors Unity/Godot treating a console clear as a
	// buffer-local meta op). Structured error: invalid_parameter (malformed body).
	Registry.Register(
		TEXT("unreal_open_mcp_console_clear_logs"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}
			const int32 Removed = FUnrealOpenMcpLogCollector::Get().Clear();
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("removed"), Removed);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		});

	// =========================================================================
	// unreal_open_mcp_console_run_command — run a console command.
	// =========================================================================
	//
	// `command` (required): the console command line (e.g. 'stat fps', 'r.
	// ScreenPercentage 50'). Executed via GEngine->Exec against the editor
	// world; output is captured. Result: { command, output, handled } where
	// `handled` is Exec's return (true when a command handler consumed it).
	//
	// Mutating (a console command can change project/editor/render state). The
	// gate's mandatory `paths_hint` (project / map scope) is enforced by the
	// dispatcher BEFORE the handler runs. This is a destructive surface — the
	// tool description says so; the gate is the safeguard, not a per-command
	// allow-list. Structured errors:
	//   - invalid_parameter  — malformed body
	//   - missing_parameter  — `command` absent/empty
	//   - editor_unavailable — no GEngine / no editor world
	Registry.Register(
		TEXT("unreal_open_mcp_console_run_command"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			TSharedPtr<FJsonObject> Args = ParseBody(Body);
			if (!Args.IsValid())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("invalid_parameter"),
					TEXT("Request body was not a valid JSON object."));
			}

			const FString Command = Args->HasTypedField<EJson::String>(TEXT("command"))
				? Args->GetStringField(TEXT("command"))
				: FString();
			if (Command.TrimStartAndEnd().IsEmpty())
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("missing_parameter"),
					TEXT("'command' is required and must be a non-empty console command string."));
			}

			if (GEngine == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("editor_unavailable"),
					TEXT("No engine is available (GEngine is null)."));
			}
			UWorld* World = FUnrealOpenMcpObjectRef::GetEditorWorld();
			if (World == nullptr)
			{
				return FUnrealOpenMcpToolDispatchResult::Fail(
					TEXT("editor_unavailable"),
					TEXT("No editor world is available. Open a level in the Unreal Editor."));
			}

			// Capture the command's own output — everything Exec writes to the
			// Ar it is handed.
			FConsoleCaptureDevice Capture;
			const bool bHandled = GEngine->Exec(World, *Command, Capture);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("command"), Command);
			Result->SetStringField(TEXT("output"), Capture.Captured);
			Result->SetBoolField(TEXT("handled"), bHandled);
			return FUnrealOpenMcpToolDispatchResult::Ok(
				WriteJson(MakeShared<FJsonValueObject>(Result)));
		}, FUnrealOpenMcpToolMetadata::Mutating());
}
