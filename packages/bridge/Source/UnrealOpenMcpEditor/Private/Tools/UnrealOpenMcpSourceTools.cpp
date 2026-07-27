// Source tool family — see header for the jail semantics, the read-only
// classification, and the Unity/Unreal-MCP fidelity notes.
#include "Tools/UnrealOpenMcpSourceTools.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace FUnrealOpenMcpSourceTools
{
	// --- Jail + path helpers --------------------------------------------------------------------

	FString GetProjectSourceRoot()
	{
		FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Source"));
		FPaths::NormalizeDirectoryName(Root);
		return Root;
	}

	FJailedPath ResolveJailedPath(const FString& JailRoot, const FString& InPath)
	{
		FJailedPath Out;

		FString Root = FPaths::ConvertRelativePathToFull(JailRoot);
		FPaths::NormalizeDirectoryName(Root);

		FString Cleaned = InPath;
		Cleaned.TrimStartAndEndInline();
		if (Cleaned.IsEmpty())
		{
			Out.Error = TEXT("path is empty.");
			return Out;
		}
		FPaths::NormalizeFilename(Cleaned); // backslashes -> forward slashes

		const FString Combined = FPaths::IsRelative(Cleaned) ? (Root / Cleaned) : Cleaned;
		FString Full = FPaths::ConvertRelativePathToFull(Combined); // collapses '.' and '..'
		FPaths::NormalizeFilename(Full);

		// Path-level jail: after collapsing traversal, the result must be the root itself or under it.
		if (!Full.Equals(Root, ESearchCase::IgnoreCase) && !FPaths::IsUnderDirectory(Full, Root))
		{
			Out.Error = FString::Printf(TEXT("'%s' escapes the project Source/ jail."), *InPath);
			return Out;
		}

		// Best-effort junction/symlink jail: resolve the deepest EXISTING ancestor's real on-disk path
		// (fixes case and, where the platform supports it, follows reparse points) and re-check
		// containment, so a junction placed INSIDE Source/ that targets outside is still rejected.
		// NOTE: this is a PATH-level check, so a TOCTOU window exists between it and the later file op
		// (a junction could be swapped in after we validate). UE's file API offers no handle-based
		// re-verify, and the documented threat model (a local AI editing project source) accepts it.
		{
			IFileManager& FM = IFileManager::Get();
			FString Existing = Full;
			while (!Existing.Equals(Root, ESearchCase::IgnoreCase)
				&& !FM.FileExists(*Existing) && !FM.DirectoryExists(*Existing))
			{
				const FString Parent = FPaths::GetPath(Existing);
				if (Parent.IsEmpty() || Parent.Equals(Existing, ESearchCase::IgnoreCase))
				{
					break;
				}
				Existing = Parent;
			}
			if (FM.FileExists(*Existing) || FM.DirectoryExists(*Existing))
			{
				FString OnDisk = FPaths::ConvertRelativePathToFull(FM.GetFilenameOnDisk(*Existing));
				FPaths::NormalizeFilename(OnDisk);
				FString RootOnDisk = FPaths::ConvertRelativePathToFull(FM.GetFilenameOnDisk(*Root));
				FPaths::NormalizeFilename(RootOnDisk);
				FPaths::NormalizeDirectoryName(RootOnDisk);
				if (!OnDisk.Equals(RootOnDisk, ESearchCase::IgnoreCase) && !FPaths::IsUnderDirectory(OnDisk, RootOnDisk))
				{
					Out.Error = FString::Printf(TEXT("'%s' resolves through a link that escapes the project Source/ jail."), *InPath);
					return Out;
				}
			}
		}

		Out.FullPath = Full;
		Out.RelPath = Full;
		FPaths::MakePathRelativeTo(Out.RelPath, *(Root + TEXT("/")));

		// Reject NTFS alternate-data-stream syntax ("Foo.cpp:stream"): a ':' in the path RELATIVE to
		// the jail root would otherwise let a read/write target an ADS that survives canonicalization.
		// The drive-letter colon lives in the jail root, not the remainder, so checking RelPath is safe.
		if (Out.RelPath.Contains(TEXT(":")))
		{
			Out.FullPath.Reset();
			Out.RelPath.Reset();
			Out.Error = FString::Printf(TEXT("'%s' contains an illegal ':' (alternate data stream)."), *InPath);
			return Out;
		}

		Out.bOk = true;
		return Out;
	}

	// ---------------------------------------------------------------------------
	// Tool registration
	// ---------------------------------------------------------------------------

	namespace
	{
		/** Parse the raw POST body into a JSON object (empty body → empty object,
		 *  malformed → null). Same contract as the actor / console families. */
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

		/** Hard cap on file size we are willing to load into memory. The max_lines
		 *  cap bounds the RETURNED slice, not the read, so this guard refuses an
		 *  absurdly large file BEFORE the load. 64 MiB is far above any sane
		 *  source file. */
		static constexpr int64 MaxReadableBytes = 64ll * 1024 * 1024;

		/** Soft cap on the number of returned lines (default + hard ceiling for a
		 *  caller-supplied max_lines). Keeps a single read from flooding the LLM
		 *  context. */
		constexpr int32 DefaultMaxLines = 2000;
		constexpr int32 HardMaxLines = 20000;

		/** Default source-file extension allow-list for source_list. Includes the
		 *  common Unreal C++ extensions; .cs is included for rare mixed trees. */
		const TArray<FString>& DefaultExtensions()
		{
			static const TArray<FString> Exts = { TEXT("h"), TEXT("hpp"), TEXT("c"), TEXT("cc"), TEXT("cpp"), TEXT("cs") };
			return Exts;
		}
	}

	void Register(FUnrealOpenMcpToolRegistry& Registry)
	{
		// =========================================================================
		// unreal_open_mcp_source_read — read a source file with optional line slice.
		// =========================================================================
		//
		// Args: `path` (required, Source-relative or absolute-inside-jail),
		// `start_line` (default 1, 1-based inclusive), `end_line` (default 0 = EOF),
		// `max_lines` (default 2000, soft cap). Result:
		//   { path, total_lines, start_line, end_line, truncated, lines:[{line,text}] }
		// Read-only. Structured errors: invalid_parameter, missing_parameter,
		// path_escapes_jail, file_not_found, not_a_file, read_failed.
		Registry.Register(
			TEXT("unreal_open_mcp_source_read"),
			[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
			{
				TSharedPtr<FJsonObject> Args = ParseBody(Body);
				if (!Args.IsValid())
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						TEXT("Request body was not a valid JSON object."));
				}

				if (!Args->HasTypedField<EJson::String>(TEXT("path")))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("missing_parameter"),
						TEXT("'path' is required and must be a non-empty source file path."));
				}
				const FString Path = Args->GetStringField(TEXT("path"));

				const FString Root = GetProjectSourceRoot();
				const FJailedPath Jailed = ResolveJailedPath(Root, Path);
				if (!Jailed.bOk)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("path_escapes_jail"), Jailed.Error);
				}

				if (FPaths::DirectoryExists(Jailed.FullPath))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("not_a_file"),
						FString::Printf(TEXT("'%s' is a directory, not a file."), *Jailed.RelPath));
				}
				if (!FPaths::FileExists(Jailed.FullPath))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("file_not_found"),
						FString::Printf(TEXT("No source file at '%s'."), *Jailed.RelPath));
				}

				// Refuse absurdly large files BEFORE loading the whole thing.
				const int64 OnDiskSize = IFileManager::Get().FileSize(*Jailed.FullPath);
				if (OnDiskSize > MaxReadableBytes)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("read_failed"),
						FString::Printf(
							TEXT("'%s' is %lld bytes; refusing to read files larger than %lld bytes."),
							*Jailed.RelPath, OnDiskSize, MaxReadableBytes));
				}

				FString Content;
				if (!FFileHelper::LoadFileToString(Content, *Jailed.FullPath))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("read_failed"),
						FString::Printf(TEXT("Failed to read '%s'."), *Jailed.RelPath));
				}

				TArray<FString> AllLines;
				Content.ParseIntoArrayLines(AllLines, /*InCullEmpty*/ false);
				// ParseIntoArrayLines appends a phantom trailing empty element for a newline-terminated
				// file ("A\nB\n" -> ["A","B",""]); drop it so total_lines counts real lines (not lines+1)
				// and the 1-based window below addresses only real lines.
				if (Content.EndsWith(TEXT("\n")) && AllLines.Num() > 0 && AllLines.Last().IsEmpty())
				{
					AllLines.Pop();
				}
				const int32 TotalLines = AllLines.Num();

				const bool bWindowed = Args->HasTypedField<EJson::Number>(TEXT("start_line"))
					|| Args->HasTypedField<EJson::Number>(TEXT("end_line"));
				int32 StartLine = 1;
				int32 EndLine = TotalLines;
				if (bWindowed)
				{
					// Clamp in int64 BEFORE narrowing — a huge value would otherwise wrap to a small
					// in-range int32 before the clamp ever ran.
					StartLine = static_cast<int32>(FMath::Clamp<int64>(
						Args->HasTypedField<EJson::Number>(TEXT("start_line"))
							? static_cast<int64>(Args->GetNumberField(TEXT("start_line")))
							: 1,
						1, FMath::Max(1, TotalLines)));
					EndLine = static_cast<int32>(FMath::Clamp<int64>(
						Args->HasTypedField<EJson::Number>(TEXT("end_line"))
							? static_cast<int64>(Args->GetNumberField(TEXT("end_line")))
							: TotalLines,
						StartLine, FMath::Max(1, TotalLines)));
				}

				// Soft cap on the number of returned lines.
				int32 MaxLines = DefaultMaxLines;
				if (Args->HasTypedField<EJson::Number>(TEXT("max_lines")))
				{
					MaxLines = static_cast<int32>(FMath::Clamp<int64>(
						static_cast<int64>(Args->GetNumberField(TEXT("max_lines"))), 1, HardMaxLines));
				}
				bool bTruncated = false;
				int32 ReturnedStart = StartLine;
				int32 ReturnedEnd = EndLine;
				if (EndLine - StartLine + 1 > MaxLines)
				{
					ReturnedEnd = StartLine + MaxLines - 1;
					bTruncated = true;
				}

				TArray<TSharedPtr<FJsonValue>> LinesJson;
				LinesJson.Reserve(ReturnedEnd - ReturnedStart + 1);
				for (int32 Index = ReturnedStart - 1; Index < ReturnedEnd && Index < TotalLines; ++Index)
				{
					TSharedRef<FJsonObject> LineObj = MakeShared<FJsonObject>();
					LineObj->SetNumberField(TEXT("line"), Index + 1);
					LineObj->SetStringField(TEXT("text"), AllLines[Index]);
					LinesJson.Add(MakeShared<FJsonValueObject>(LineObj));
				}

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("path"), Jailed.RelPath);
				Result->SetNumberField(TEXT("total_lines"), TotalLines);
				Result->SetNumberField(TEXT("start_line"), ReturnedStart);
				Result->SetNumberField(TEXT("end_line"), ReturnedEnd);
				Result->SetBoolField(TEXT("truncated"), bTruncated);
				Result->SetArrayField(TEXT("lines"), LinesJson);
				return FUnrealOpenMcpToolDispatchResult::Ok(
					WriteJson(MakeShared<FJsonValueObject>(Result)));
			});

		// =========================================================================
		// unreal_open_mcp_source_list — enumerate source files under Source/.
		// =========================================================================
		//
		// Args: `module` (optional folder under Source/), `recursive` (default
		// true), `extensions` (optional array; default .h/.hpp/.c/.cc/.cpp/.cs).
		// Result: { root, files:[{path,bytes}], count, total_bytes }. Read-only.
		// Structured errors: invalid_parameter, path_escapes_jail, module_not_found.
		Registry.Register(
			TEXT("unreal_open_mcp_source_list"),
			[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
			{
				TSharedPtr<FJsonObject> Args = ParseBody(Body);
				if (!Args.IsValid())
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						TEXT("Request body was not a valid JSON object."));
				}

				const FString Root = GetProjectSourceRoot();
				FString BaseDir = Root;
				FString BaseRel;
				FString ModuleRootLabel = TEXT("Source");

				if (Args->HasTypedField<EJson::String>(TEXT("module")))
				{
					const FString Module = Args->GetStringField(TEXT("module"));
					if (!Module.IsEmpty())
					{
						const FJailedPath Jailed = ResolveJailedPath(Root, Module);
						if (!Jailed.bOk)
						{
							return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("path_escapes_jail"), Jailed.Error);
						}
						if (!FPaths::DirectoryExists(Jailed.FullPath))
						{
							return FUnrealOpenMcpToolDispatchResult::Fail(
								TEXT("module_not_found"),
								FString::Printf(TEXT("Module folder '%s' does not exist under Source/."), *Module));
						}
						BaseDir = Jailed.FullPath;
						BaseRel = Jailed.RelPath;
						ModuleRootLabel = FString::Printf(TEXT("Source/%s"), *BaseRel);
					}
				}

				// Resolve the extensions filter (default allow-list when omitted/empty).
				TArray<FString> Extensions;
				const TArray<TSharedPtr<FJsonValue>>* ExtArray = nullptr;
				if (Args->HasTypedField<EJson::Array>(TEXT("extensions"))
					&& Args->TryGetArrayField(TEXT("extensions"), ExtArray)
					&& ExtArray != nullptr)
				{
					for (const TSharedPtr<FJsonValue>& Value : *ExtArray)
					{
						FString Ext;
						if (Value.IsValid() && Value->TryGetString(Ext) && !Ext.IsEmpty())
						{
							Extensions.AddUnique(Ext.Replace(TEXT("."), TEXT("")).ToLower());
						}
					}
				}
				if (Extensions.Num() == 0)
				{
					Extensions = DefaultExtensions();
				}

				const bool bRecursive = Args->HasTypedField<EJson::Bool>(TEXT("recursive"))
					? Args->GetBoolField(TEXT("recursive"))
					: true;

				IFileManager& FM = IFileManager::Get();
				TArray<FString> Found;
				for (const FString& Ext : Extensions)
				{
					TArray<FString> Matches;
					const FString Wildcard = FString::Printf(TEXT("*.%s"), *Ext);
					if (bRecursive)
					{
						FM.FindFilesRecursive(Matches, *BaseDir, *Wildcard, /*Files*/ true, /*Dirs*/ false, /*bClearFileNames*/ false);
					}
					else
					{
						TArray<FString> Names;
						FM.FindFiles(Names, *(BaseDir / Wildcard), /*Files*/ true, /*Dirs*/ false);
						for (const FString& Name : Names)
						{
							Matches.Add(BaseDir / Name);
						}
					}
					Found.Append(Matches);
				}

				Found.Sort();
				int64 TotalBytes = 0;
				TArray<TSharedPtr<FJsonValue>> Files;
				Files.Reserve(Found.Num());
				for (const FString& Full : Found)
				{
					FString Rel = FPaths::ConvertRelativePathToFull(Full);
					FPaths::NormalizeFilename(Rel);
					FPaths::MakePathRelativeTo(Rel, *(Root + TEXT("/")));
					const int64 Bytes = FM.FileSize(*Full);
					if (Bytes >= 0)
					{
						TotalBytes += Bytes;
					}
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("path"), Rel);
					Entry->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
					Files.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("root"), ModuleRootLabel);
				Result->SetNumberField(TEXT("count"), Files.Num());
				Result->SetNumberField(TEXT("total_bytes"), static_cast<double>(TotalBytes));
				Result->SetArrayField(TEXT("files"), Files);
				return FUnrealOpenMcpToolDispatchResult::Ok(
					WriteJson(MakeShared<FJsonValueObject>(Result)));
			});
	}
} // namespace FUnrealOpenMcpSourceTools
