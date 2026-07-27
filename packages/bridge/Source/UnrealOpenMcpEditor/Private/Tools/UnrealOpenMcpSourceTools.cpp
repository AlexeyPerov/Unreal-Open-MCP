// Source tool family — see header for the jail semantics, the read-only
// classification, and the Unity/Unreal-MCP fidelity notes.
#include "Tools/UnrealOpenMcpSourceTools.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"

#include "Misc/App.h" // FApp::GetProjectName for the default create-class module
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
// P7.3 — diagnostic parser uses FRegexPattern / FRegexMatcher (the
// Internationalization module's regex, available without an extra Build.cs dep
// because it is pulled in transitively by Core/Engine). MSVC + clang lines.
#include "Internationalization/Regex.h"
// P7.3 — UBT launch (FPlatformProcess::ExecProcess + GetBinariesSubdirectory)
// and timing (FPlatformTime::Seconds).
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// P7.3 — Live Coding is an interactive-only fast path that patches the running
// editor's module DLL in place (the only way to apply C++ changes without a
// full relink while the editor holds the DLL). The module is gated behind a
// compile-time define so a host without the LiveCoding module still builds the
// bridge — the source_compile handler falls through to the UBT path when LC is
// absent at compile time OR unavailable at runtime. Mirrors the Unreal-MCP
// behavior reference's WITH_UNREAL_MCP_LIVE_CODING guard pattern.
#if WITH_UNREAL_MCP_LIVE_CODING
#include "ILiveCodingModule.h"
#include "Modules/ModuleManager.h"
#endif

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
	// P7.3 — compiler-diagnostic parser
	// ---------------------------------------------------------------------------

	void ParseDiagnostics(const FString& BuildOutput, TArray<FSourceDiagnostic>& OutDiagnostics)
	{
		OutDiagnostics.Reset();

		TArray<FString> Lines;
		BuildOutput.ParseIntoArrayLines(Lines, /*InCullEmpty*/ false);

		// MSVC:  C:\path\File.cpp(42): error C2065: 'Foo': undeclared identifier
		//        File.cpp(42,5): warning C4101: ...   (newer toolchains may add a column)
		//        File.cpp(1): fatal error C1083: ...   (a missing/typo'd #include — the optional
		//        "fatal " prefix is consumed and the severity normalized to "error")
		const FRegexPattern MsvcPattern(TEXT("^\\s*(.+?)\\((\\d+)(?:,\\d+)?\\)\\s*:\\s*(?:fatal\\s+)?(error|warning)\\s+(.+?)\\s*$"));
		// clang: File.cpp:42:5: error: ...   (also "File.cpp:1:10: fatal error: 'X.h' file not found")
		const FRegexPattern ClangPattern(TEXT("^\\s*(.+?):(\\d+):(?:\\d+:)?\\s*(?:fatal\\s+)?(error|warning):\\s*(.+?)\\s*$"));

		TSet<FString> Seen;
		auto AddMatch = [&OutDiagnostics, &Seen](const FString& File, const FString& LineStr, const FString& Sev, const FString& Msg)
		{
			FSourceDiagnostic D;
			D.File = File;
			D.File.TrimStartAndEndInline();
			D.Line = FCString::Atoi(*LineStr);
			D.Severity = Sev.ToLower();
			D.Message = Msg;
			D.Message.TrimStartAndEndInline();
			// Dedupe key — a UBT run often emits the same diagnostic on stdout
			// AND stderr, and the same error from a header surfaces once per
			// including cpp. Collapse exact (file,line,severity,message) dupes.
			const FString Key = FString::Printf(TEXT("%s|%d|%s|%s"), *D.File, D.Line, *D.Severity, *D.Message);
			if (!Seen.Contains(Key))
			{
				Seen.Add(Key);
				OutDiagnostics.Add(MoveTemp(D));
			}
		};

		for (const FString& Line : Lines)
		{
			if (Line.IsEmpty())
			{
				continue;
			}
			FRegexMatcher Msvc(MsvcPattern, Line);
			if (Msvc.FindNext())
			{
				AddMatch(Msvc.GetCaptureGroup(1), Msvc.GetCaptureGroup(2), Msvc.GetCaptureGroup(3), Msvc.GetCaptureGroup(4));
				continue;
			}
			FRegexMatcher Clang(ClangPattern, Line);
			if (Clang.FindNext())
			{
				AddMatch(Clang.GetCaptureGroup(1), Clang.GetCaptureGroup(2), Clang.GetCaptureGroup(3), Clang.GetCaptureGroup(4));
			}
		}
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

		// --- P7.2 create-class template helpers ------------------------------------

		/** Neutral copyright/file-header banner for generated scaffolds. Deliberately
		 *  NOT the third-party Apache header the Unreal-MCP reference emits — the
		 *  project owns these files, so a permissive MIT-style placeholder is the
		 *  right default. Agents can overwrite it via source_update. */
		const TCHAR* GeneratedFileHeaderComment()
		{
			return TEXT("// Generated by Unreal Open MCP. Edit freely; no reserved rights claimed.\n");
		}

		/** True if @p Name is a legal C++ identifier (so a generated class name can
		 *  never inject C++ syntax into the scaffold). Letter/digit/underscore, no
		 *  leading digit. */
		bool IsValidIdentifier(const FString& Name)
		{
			if (Name.IsEmpty())
			{
				return false;
			}
			for (int32 Index = 0; Index < Name.Len(); ++Index)
			{
				const TCHAR Ch = Name[Index];
				const bool bAlpha = (Ch >= TEXT('A') && Ch <= TEXT('Z'))
					|| (Ch >= TEXT('a') && Ch <= TEXT('z'))
					|| Ch == TEXT('_');
				const bool bDigit = (Ch >= TEXT('0') && Ch <= TEXT('9'));
				if (!bAlpha && !(bDigit && Index > 0))
				{
					return false;
				}
			}
			return true;
		}

		/** Derive the `<MODULENAME>_API` dllexport macro a class in @p ModuleName
		 *  publishes. Uppercased module name + "_API", matching UBT's convention. */
		FString ModuleApiMacro(const FString& ModuleName)
		{
			return ModuleName.ToUpper() + TEXT("_API");
		}

		/** Resolved class-scaffold parameters for one of the supported parent kinds.
		 *  The prefix is derived from the parent (U/A/F), the parent symbol is the
		 *  base class to inherit from, the parent header is the include path (empty
		 *  for a plain non-UCLASS class). */
		struct FClassTemplate
		{
			FString Prefix;        // "U" / "A" / "F"
			FString ParentSymbol;  // "UObject" / "AActor" / "UActorComponent" / "" (plain)
			FString ParentHeader;  // include path, empty for plain
			FString KindLabel;     // display label returned to the caller
			bool bUClass = false;
		};

		/** Resolve the parent-kind token to a scaffold template. Accepts both the
		 *  bare ("Actor") and prefixed ("AActor") forms for the supported kinds, plus
		 *  None/Empty/Plain for a non-UCLASS class. Returns false + sets @p OutError
		 *  for an unsupported kind (the caller surfaces the supported list). */
		bool ResolveClassTemplate(const FString& InParent, FClassTemplate& Out, FString& OutError)
		{
			const FString P = InParent.TrimStartAndEnd();
			if (P.IsEmpty()
				|| P.Equals(TEXT("UObject"), ESearchCase::IgnoreCase)
				|| P.Equals(TEXT("Object"), ESearchCase::IgnoreCase))
			{
				Out = { TEXT("U"), TEXT("UObject"), TEXT("UObject/NoExportTypes.h"), TEXT("UObject"), true };
				return true;
			}
			if (P.Equals(TEXT("AActor"), ESearchCase::IgnoreCase)
				|| P.Equals(TEXT("Actor"), ESearchCase::IgnoreCase))
			{
				Out = { TEXT("A"), TEXT("AActor"), TEXT("GameFramework/Actor.h"), TEXT("Actor"), true };
				return true;
			}
			if (P.Equals(TEXT("UActorComponent"), ESearchCase::IgnoreCase)
				|| P.Equals(TEXT("ActorComponent"), ESearchCase::IgnoreCase))
			{
				Out = { TEXT("U"), TEXT("UActorComponent"), TEXT("Components/ActorComponent.h"), TEXT("ActorComponent"), true };
				return true;
			}
			if (P.Equals(TEXT("None"), ESearchCase::IgnoreCase)
				|| P.Equals(TEXT("Empty"), ESearchCase::IgnoreCase)
				|| P.Equals(TEXT("Plain"), ESearchCase::IgnoreCase))
			{
				Out = { TEXT("F"), FString(), FString(), TEXT("None"), false };
				return true;
			}
			OutError = FString::Printf(
				TEXT("Unsupported parent_class '%s'. Supported: UObject (default), Actor, ActorComponent, None (a plain non-UCLASS class)."),
				*InParent);
			return false;
		}

		/** Render the header file for a class scaffold. The UCLASS form emits
		 *  GENERATED_BODY() + the generated-header include; the plain form emits an
		 *  empty non-UCLASS class. */
		FString BuildHeader(const FClassTemplate& Tpl, const FString& ClassName, const FString& ApiMacro)
		{
			const FString Symbol = Tpl.Prefix + ClassName;
			if (Tpl.bUClass)
			{
				return FString::Printf(
					TEXT("%s#pragma once\n\n")
					TEXT("#include \"CoreMinimal.h\"\n")
					TEXT("#include \"%s\"\n")
					TEXT("#include \"%s.generated.h\"\n\n")
					TEXT("UCLASS()\n")
					TEXT("class %s %s : public %s\n")
					TEXT("{\n")
					TEXT("\tGENERATED_BODY()\n")
					TEXT("};\n"),
					GeneratedFileHeaderComment(),
					*Tpl.ParentHeader, *ClassName, *ApiMacro, *Symbol, *Tpl.ParentSymbol);
			}
			return FString::Printf(
				TEXT("%s#pragma once\n\n")
				TEXT("#include \"CoreMinimal.h\"\n\n")
				TEXT("class %s %s\n")
				TEXT("{\n")
				TEXT("};\n"),
				GeneratedFileHeaderComment(), *ApiMacro, *Symbol);
		}

		/** Render the cpp file for a class scaffold — just the header include. The
		 *  method bodies are the agent's job (source_update + the P7.3 compile
		 *  feedback loop). */
		FString BuildCpp(const FString& ClassName)
		{
			return FString::Printf(
				TEXT("%s#include \"%s.h\"\n"),
				GeneratedFileHeaderComment(), *ClassName);
		}

		/** Write @p Text to @p FullPath as BOM-less UTF-8. Returns false on failure. */
		bool WriteUtf8(const FString& FullPath, const FString& Text)
		{
			return FFileHelper::SaveStringToFile(
				Text, *FullPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
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

		// =========================================================================
		// unreal_open_mcp_source_create_class — scaffold a header + cpp under an
		// existing module folder from parent-kind templates.
		// =========================================================================
		//
		// Args: `class_name` (required, bare name WITHOUT U/A/F prefix),
		// `parent_class` (default "UObject" — one of UObject / Actor /
		// ActorComponent / None), `module` (default the primary project module),
		// `force` (default false). Mutating: gate Enforce, `paths_hint` required
		// (the dispatcher enforces the hint BEFORE this handler runs).
		// Result: { class_name, module, parent_class, header, cpp, is_uclass }.
		// Structured errors: invalid_parameter, missing_parameter, module_not_found,
		// already_exists, path_escapes_jail, write_failed.
		Registry.Register(
			TEXT("unreal_open_mcp_source_create_class"),
			[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
			{
				TSharedPtr<FJsonObject> Args = ParseBody(Body);
				if (!Args.IsValid())
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						TEXT("Request body was not a valid JSON object."));
				}

				const FString ClassName = Args->HasTypedField<EJson::String>(TEXT("class_name"))
					? Args->GetStringField(TEXT("class_name")) : FString();
				if (!IsValidIdentifier(ClassName))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						FString::Printf(
							TEXT("'class_name' must be a valid C++ identifier (letters/digits/underscore, no leading digit; no U/A/F prefix). Got '%s'."),
							*ClassName));
				}

				FString Module = Args->HasTypedField<EJson::String>(TEXT("module"))
					? Args->GetStringField(TEXT("module")) : FString();
				if (Module.IsEmpty())
				{
					Module = FApp::GetProjectName();
				}
				if (!IsValidIdentifier(Module))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						FString::Printf(TEXT("'%s' is not a valid module name."), *Module));
				}

				const FString ParentToken = Args->HasTypedField<EJson::String>(TEXT("parent_class"))
					? Args->GetStringField(TEXT("parent_class")) : FString();
				FClassTemplate Tpl;
				FString TemplateError;
				if (!ResolveClassTemplate(ParentToken, Tpl, TemplateError))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("invalid_parameter"), TemplateError);
				}

				const FString Root = GetProjectSourceRoot();
				const FJailedPath ModuleDir = ResolveJailedPath(Root, Module);
				if (!ModuleDir.bOk)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("path_escapes_jail"), ModuleDir.Error);
				}
				if (!FPaths::DirectoryExists(ModuleDir.FullPath))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("module_not_found"),
						FString::Printf(
							TEXT("Module folder '%s' does not exist under Source/. Create the module (Build.cs + folder) first."),
							*Module));
				}

				const FJailedPath HeaderPath = ResolveJailedPath(Root, Module / (ClassName + TEXT(".h")));
				const FJailedPath CppPath = ResolveJailedPath(Root, Module / (ClassName + TEXT(".cpp")));
				if (!HeaderPath.bOk)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("path_escapes_jail"), HeaderPath.Error);
				}
				if (!CppPath.bOk)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("path_escapes_jail"), CppPath.Error);
				}

				const bool bForce = Args->HasTypedField<EJson::Bool>(TEXT("force"))
					? Args->GetBoolField(TEXT("force")) : false;
				const bool bHeaderExists = FPaths::FileExists(HeaderPath.FullPath);
				const bool bCppExists = FPaths::FileExists(CppPath.FullPath);
				if (!bForce && (bHeaderExists || bCppExists))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("already_exists"),
						FString::Printf(
							TEXT("'%s.h'/'%s.cpp' already exist in module '%s'. Pass \"force\":true to overwrite."),
							*ClassName, *ClassName, *Module));
				}

				const FString ApiMacro = ModuleApiMacro(Module);
				const FString HeaderText = BuildHeader(Tpl, ClassName, ApiMacro);
				const FString CppText = BuildCpp(ClassName);

				// Atomic-ish scaffold: write the header first, then the cpp; roll the
				// header back if the cpp write fails so a stranded half-scaffold never
				// blocks a clean retry without force.
				if (!WriteUtf8(HeaderPath.FullPath, HeaderText))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("write_failed"),
						FString::Printf(TEXT("Failed to write header '%s'."), *HeaderPath.RelPath));
				}
				if (!WriteUtf8(CppPath.FullPath, CppText))
				{
					IFileManager::Get().Delete(*HeaderPath.FullPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("write_failed"),
						FString::Printf(TEXT("Failed to write cpp '%s' (header rolled back)."), *CppPath.RelPath));
				}

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("class_name"), Tpl.Prefix + ClassName);
				Result->SetStringField(TEXT("module"), Module);
				Result->SetStringField(TEXT("parent_class"), Tpl.bUClass ? Tpl.ParentSymbol : TEXT("(none)"));
				Result->SetStringField(TEXT("header"), HeaderPath.RelPath);
				Result->SetStringField(TEXT("cpp"), CppPath.RelPath);
				Result->SetBoolField(TEXT("is_uclass"), Tpl.bUClass);
				return FUnrealOpenMcpToolDispatchResult::Ok(
					WriteJson(MakeShared<FJsonValueObject>(Result)));
			},
			FUnrealOpenMcpToolMetadata::Mutating());

		// =========================================================================
		// unreal_open_mcp_source_update — full-file replace or 1-based inclusive
		// line-range splice on an existing source file.
		// =========================================================================
		//
		// Args: `path` (required), `content` (required), `start_line` + `end_line`
		// (together — splice content over that 1-based inclusive range). Mutating:
		// gate Enforce, `paths_hint` required.
		// Result: { path, mode, bytes_written, lines_written }.
		// Structured errors: invalid_parameter, missing_parameter, file_not_found,
		// invalid_line_range, path_escapes_jail, write_failed.
		Registry.Register(
			TEXT("unreal_open_mcp_source_update"),
			[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
			{
				TSharedPtr<FJsonObject> Args = ParseBody(Body);
				if (!Args.IsValid())
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						TEXT("Request body was not a valid JSON object."));
				}

				const FString Path = Args->HasTypedField<EJson::String>(TEXT("path"))
					? Args->GetStringField(TEXT("path")) : FString();
				if (Path.IsEmpty())
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("missing_parameter"),
						TEXT("'path' is required and must be a non-empty source file path."));
				}
				if (!Args->HasField(TEXT("content")))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("missing_parameter"),
						TEXT("'content' is required."));
				}
				// `content` may legitimately be an empty string — but JSON null or a
				// non-string type is a caller error.
				if (Args->HasTypedField<EJson::Null>(TEXT("content")))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						TEXT("'content' must be a string (got null)."));
				}
				FString NewContent;
				if (!Args->TryGetStringField(TEXT("content"), NewContent))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						TEXT("'content' must be a string."));
				}

				const FString Root = GetProjectSourceRoot();
				const FJailedPath Jailed = ResolveJailedPath(Root, Path);
				if (!Jailed.bOk)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("path_escapes_jail"), Jailed.Error);
				}
				if (!FPaths::FileExists(Jailed.FullPath))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("file_not_found"),
						FString::Printf(
							TEXT("No source file at '%s' (use source_create_class to create new files)."),
							*Jailed.RelPath));
				}

				const bool bHasStart = Args->HasTypedField<EJson::Number>(TEXT("start_line"));
				const bool bHasEnd = Args->HasTypedField<EJson::Number>(TEXT("end_line"));
				const bool bRange = bHasStart || bHasEnd;
				FString Final;
				FString Mode;
				if (bRange)
				{
					if (!bHasStart || !bHasEnd)
					{
						return FUnrealOpenMcpToolDispatchResult::Fail(
							TEXT("invalid_parameter"),
							TEXT("A line-range update requires BOTH 'start_line' and 'end_line'."));
					}

					FString Existing;
					if (!FFileHelper::LoadFileToString(Existing, *Jailed.FullPath))
					{
						return FUnrealOpenMcpToolDispatchResult::Fail(
							TEXT("write_failed"),
							FString::Printf(TEXT("Failed to read '%s' for splice."), *Jailed.RelPath));
					}
					// Preserve the file's dominant line ending so a one-line splice on a
					// CRLF file is a one-line diff, not a whole-file CRLF->LF rewrite.
					const TCHAR* Eol = Existing.Contains(TEXT("\r\n")) ? TEXT("\r\n") : TEXT("\n");
					const bool bTrailingNewline = Existing.EndsWith(TEXT("\n"));
					TArray<FString> Lines;
					Existing.ParseIntoArrayLines(Lines, /*InCullEmpty*/ false);
					// ParseIntoArrayLines appends a phantom trailing empty element for a
					// newline-terminated file; drop it so the addressable range is the
					// real line count and the trailing-EOL re-add does not double the
					// final newline.
					if (bTrailingNewline && Lines.Num() > 0 && Lines.Last().IsEmpty())
					{
						Lines.Pop();
					}

					// Validate the range in int64 BEFORE narrowing — a huge value would
					// otherwise wrap to a small in-range int32 and silently splice the
					// WRONG lines. A silent mis-edit on the write path is worse than the
					// read path's over-read, so reject here rather than clamp.
					const int64 StartLine64 = static_cast<int64>(Args->GetNumberField(TEXT("start_line")));
					const int64 EndLine64 = static_cast<int64>(Args->GetNumberField(TEXT("end_line")));
					if (StartLine64 < 1 || EndLine64 < StartLine64
						|| StartLine64 > Lines.Num() || EndLine64 > Lines.Num())
					{
						return FUnrealOpenMcpToolDispatchResult::Fail(
							TEXT("invalid_line_range"),
							FString::Printf(
								TEXT("Invalid line range [%lld..%lld] for '%s' (%d line(s))."),
								StartLine64, EndLine64, *Jailed.RelPath, Lines.Num()));
					}
					const int32 StartLine = static_cast<int32>(StartLine64);
					const int32 EndLine = static_cast<int32>(EndLine64);

					TArray<FString> Replacement;
					NewContent.ParseIntoArrayLines(Replacement, /*InCullEmpty*/ false);
					// Mirror the phantom-trailing-empty drop on the REPLACEMENT side: a
					// newline-terminated `content` would otherwise contribute its own
					// EOL via the Join AND re-add the file's trailing EOL, splicing a
					// spurious blank line per edit. AI callers commonly send
					// newline-terminated content, so guard the write path symmetrically.
					if (NewContent.EndsWith(TEXT("\n")) && Replacement.Num() > 0 && Replacement.Last().IsEmpty())
					{
						Replacement.Pop();
					}

					TArray<FString> Result;
					Result.Reserve(Lines.Num() + Replacement.Num());
					Result.Append(Lines.GetData(), StartLine - 1);
					Result.Append(Replacement);
					for (int32 Index = EndLine; Index < Lines.Num(); ++Index)
					{
						Result.Add(Lines[Index]);
					}
					Final = FString::Join(Result, Eol);
					if (bTrailingNewline)
					{
						Final += Eol;
					}
					Mode = TEXT("range");
				}
				else
				{
					Final = NewContent;
					Mode = TEXT("full");
				}

				if (!WriteUtf8(Jailed.FullPath, Final))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("write_failed"),
						FString::Printf(TEXT("Failed to write '%s'."), *Jailed.RelPath));
				}

				const int32 BytesWritten = FTCHARToUTF8(*Final).Length();
				TArray<FString> FinalLines;
				Final.ParseIntoArrayLines(FinalLines, /*InCullEmpty*/ false);
				if (Final.EndsWith(TEXT("\n")) && FinalLines.Num() > 0 && FinalLines.Last().IsEmpty())
				{
					FinalLines.Pop();
				}

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("path"), Jailed.RelPath);
				Result->SetStringField(TEXT("mode"), Mode);
				Result->SetNumberField(TEXT("bytes_written"), static_cast<double>(BytesWritten));
				Result->SetNumberField(TEXT("lines_written"), static_cast<double>(FinalLines.Num()));
				return FUnrealOpenMcpToolDispatchResult::Ok(
					WriteJson(MakeShared<FJsonValueObject>(Result)));
			},
			FUnrealOpenMcpToolMetadata::Mutating());

		// =========================================================================
		// unreal_open_mcp_source_delete — delete a single source file (refuses
		// directories). Destructive + not undoable from MCP.
		// =========================================================================
		//
		// Args: `path` (required). Mutating: gate Enforce, `paths_hint` required.
		// Result: { path, deleted: true }.
		// Structured errors: invalid_parameter, missing_parameter, file_not_found,
		// is_directory, path_escapes_jail, delete_failed.
		Registry.Register(
			TEXT("unreal_open_mcp_source_delete"),
			[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
			{
				TSharedPtr<FJsonObject> Args = ParseBody(Body);
				if (!Args.IsValid())
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						TEXT("Request body was not a valid JSON object."));
				}

				const FString Path = Args->HasTypedField<EJson::String>(TEXT("path"))
					? Args->GetStringField(TEXT("path")) : FString();
				if (Path.IsEmpty())
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("missing_parameter"),
						TEXT("'path' is required and must be a non-empty source file path."));
				}

				const FString Root = GetProjectSourceRoot();
				const FJailedPath Jailed = ResolveJailedPath(Root, Path);
				if (!Jailed.bOk)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("path_escapes_jail"), Jailed.Error);
				}
				if (FPaths::DirectoryExists(Jailed.FullPath))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("is_directory"),
						FString::Printf(
							TEXT("'%s' is a directory; source_delete only removes files."),
							*Jailed.RelPath));
				}
				if (!FPaths::FileExists(Jailed.FullPath))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("file_not_found"),
						FString::Printf(TEXT("No source file at '%s'."), *Jailed.RelPath));
				}
				if (!IFileManager::Get().Delete(*Jailed.FullPath, /*RequireExists*/ false, /*EvenReadOnly*/ true))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("delete_failed"),
						FString::Printf(TEXT("Failed to delete '%s'."), *Jailed.RelPath));
				}

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("path"), Jailed.RelPath);
				Result->SetBoolField(TEXT("deleted"), true);
				return FUnrealOpenMcpToolDispatchResult::Ok(
					WriteJson(MakeShared<FJsonValueObject>(Result)));
			},
			FUnrealOpenMcpToolMetadata::Mutating());

		// =========================================================================
		// unreal_open_mcp_source_compile — compile the project's C++ and return a
		// STRUCTURED diagnostic report (the AI feedback loop).
		// =========================================================================
		//
		// Args: `target` (default `<Project>Editor`), `configuration` (default
		// `Development`), `platform` (default the host binaries subdir —
		// `FPlatformProcess::GetBinariesSubdirectory()`), `use_live_coding`
		// (default true). Mutating: gate Enforce, `paths_hint` required (the
		// dispatcher enforces the hint BEFORE this handler runs).
		//
		// Two paths:
		//   1. Live Coding (interactive editor + LC session live + use_live_coding
		//      true + WITH_UNREAL_MCP_LIVE_CODING compile-time guard). Patches the
		//      running module DLL in place — no relink. Returns a COARSE result
		//      enum (Success / NoChanges / Failure / ...) with NO per-diagnostic
		//      rows. On Failure or NotStarted, falls through to UBT so the agent
		//      still gets a structured report.
		//   2. UBT (the fallback + forced path). Resolves
		//      UnrealBuildTool.exe / RunUBT.sh, validates target/platform/
		//      configuration as identifier-only tokens (no arg injection), runs
		//      ExecProcess with `-project=<uproject> -WaitMutex`, parses the
		//      combined stdout+stderr with ParseDiagnostics, returns the full
		//      {diagnostics[], counts, return_code, ...} report.
		//
		// A FAILED compile is a NORMAL, expected result, NOT a transport failure.
		// The envelope stays ok:true and the result carries success:false +
		// compile_clean:false + a populated diagnostics[] so an agent reads the
		// rows, fixes via source_update / source_create_class, and recompiles.
		// Only TOOL-LEVEL errors (UBT binary missing, invalid identifier, launch
		// failure, malformed body) map to ok:false. This mirrors P6.5
		// blueprint_compile's failed-compile-as-data contract.
		//
		// success (return_code == 0) vs compile_clean (zero compiler errors) is
		// split on purpose: a loaded editor holds its module DLL, so a UBT relink
		// fails to write it (return_code != 0, success:false). But compiler errors
		// are emitted BEFORE the link stage, so a clean compile is
		// compile_clean:true even when success:false. The AI loop keys off
		// compile_clean + diagnostics.
		//
		// Result (UBT): { method:"ubt", target, configuration, platform,
		//   return_code, success, compile_clean, error_count, warning_count,
		//   duration_seconds, diagnostics:[{file,line,severity,message}],
		//   output_tail }. Result (LC): { method:"live_coding", result, success,
		//   compile_clean, error_count, warning_count, diagnostics:[] }.
		// Structured errors: invalid_parameter, ubt_not_found, ubt_launch_failed.
		Registry.Register(
			TEXT("unreal_open_mcp_source_compile"),
			[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
			{
				TSharedPtr<FJsonObject> Args = ParseBody(Body);
				if (!Args.IsValid())
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						TEXT("Request body was not a valid JSON object."));
				}

				const bool bUseLiveCoding = Args->HasTypedField<EJson::Bool>(TEXT("use_live_coding"))
					? Args->GetBoolField(TEXT("use_live_coding")) : true;

				const FString ProjectName = FApp::GetProjectName();
				FString TargetName = Args->HasTypedField<EJson::String>(TEXT("target"))
					? Args->GetStringField(TEXT("target")) : FString();
				if (TargetName.IsEmpty())
				{
					TargetName = ProjectName + TEXT("Editor");
				}

#if WITH_UNREAL_MCP_LIVE_CODING
				// Live Coding patches the RUNNING editor in place — the only way to
				// apply C++ changes without relinking the locked module DLL. Only
				// viable interactively (the console must be up); headless /
				// unattended runs (Automation, -game) fall through to UBT.
				if (bUseLiveCoding && !FApp::IsUnattended())
				{
					ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(FName(LIVE_CODING_MODULE_NAME));
					if (LiveCoding
						&& LiveCoding->IsEnabledForSession()
						&& LiveCoding->HasStarted()
						&& !LiveCoding->IsCompiling())
					{
						ELiveCodingCompileResult Result = ELiveCodingCompileResult::NotStarted;
						const bool bStarted = LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &Result);
						const bool bLcSuccess = bStarted
							&& (Result == ELiveCodingCompileResult::Success
								|| Result == ELiveCodingCompileResult::NoChanges);

						const TCHAR* ResultText = TEXT("Unknown");
						switch (Result)
						{
							case ELiveCodingCompileResult::Success:           ResultText = TEXT("Success"); break;
							case ELiveCodingCompileResult::NoChanges:         ResultText = TEXT("NoChanges"); break;
							case ELiveCodingCompileResult::InProgress:        ResultText = TEXT("InProgress"); break;
							case ELiveCodingCompileResult::CompileStillActive: ResultText = TEXT("CompileStillActive"); break;
							case ELiveCodingCompileResult::NotStarted:        ResultText = TEXT("NotStarted"); break;
							case ELiveCodingCompileResult::Failure:           ResultText = TEXT("Failure"); break;
							case ELiveCodingCompileResult::Cancelled:         ResultText = TEXT("Cancelled"); break;
						}

						TSharedRef<FJsonObject> LcResult = MakeShared<FJsonObject>();
						LcResult->SetStringField(TEXT("method"), TEXT("live_coding"));
						LcResult->SetStringField(TEXT("result"), ResultText);
						LcResult->SetBoolField(TEXT("success"), bLcSuccess);
						LcResult->SetBoolField(TEXT("compile_clean"), bLcSuccess);
						// Live Coding surfaces a coarse enum, not per-diagnostic
						// rows; on a non-success result we report a single coarse
						// error_count so an agent's "any errors?" branch still
						// fires. The UBT path carries the full report.
						LcResult->SetNumberField(TEXT("error_count"), bLcSuccess ? 0 : 1);
						LcResult->SetNumberField(TEXT("warning_count"), 0);
						LcResult->SetArrayField(TEXT("diagnostics"), {});
						// Fall through to UBT on a hard Failure OR when Compile()
						// never started (bStarted == false, leaving Result at its
						// NotStarted init): in both cases the coarse enum carries
						// no actionable diagnostics, and the agent needs the full
						// {file,line,severity,message} report to self-correct.
						if (bStarted && Result != ELiveCodingCompileResult::Failure)
						{
							return FUnrealOpenMcpToolDispatchResult::Ok(
								WriteJson(MakeShared<FJsonValueObject>(LcResult)));
						}
					}
				}
#endif

				// UBT path ----------------------------------------------------------------
				FString Platform = Args->HasTypedField<EJson::String>(TEXT("platform"))
					? Args->GetStringField(TEXT("platform")) : FString();
				if (Platform.IsEmpty())
				{
					Platform = FPlatformProcess::GetBinariesSubdirectory();
				}
				FString Configuration = Args->HasTypedField<EJson::String>(TEXT("configuration"))
					? Args->GetStringField(TEXT("configuration")) : FString();
				if (Configuration.IsEmpty())
				{
					Configuration = TEXT("Development");
				}

				// target / platform / configuration are pasted verbatim into the UBT
				// command line, so each must be a single identifier-like token (no
				// whitespace, quotes, or dash-prefixed flags) — else a value like
				// "MyGameEditor Win64 Development -Clean" would inject arbitrary UBT
				// arguments (e.g. -Clean wipes Binaries/Intermediate). Mirrors the
				// IsValidIdentifier used for generated class / module names. All
				// real targets / platforms / configs are bare identifiers.
				if (!IsValidIdentifier(TargetName))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						FString::Printf(TEXT("'%s' is not a valid build target name (identifier-only; no whitespace / flags)."), *TargetName));
				}
				if (!IsValidIdentifier(Platform))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						FString::Printf(TEXT("'%s' is not a valid build platform (identifier-only; no whitespace / flags)."), *Platform));
				}
				if (!IsValidIdentifier(Configuration))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("invalid_parameter"),
						FString::Printf(TEXT("'%s' is not a valid build configuration (identifier-only; no whitespace / flags)."), *Configuration));
				}

				FString UProject;
				if (FPaths::IsProjectFilePathSet())
				{
					UProject = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
				}
				else
				{
					UProject = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / (ProjectName + TEXT(".uproject")));
				}

#if PLATFORM_WINDOWS
				const FString Ubt = FPaths::ConvertRelativePathToFull(
					FPaths::EngineDir() / TEXT("Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe"));
#else
				const FString Ubt = FPaths::ConvertRelativePathToFull(
					FPaths::EngineDir() / TEXT("Build/BatchFiles/RunUBT.sh"));
#endif
				if (!FPaths::FileExists(Ubt))
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("ubt_not_found"),
						FString::Printf(TEXT("UnrealBuildTool not found at '%s'."), *Ubt));
				}

				const FString Params = FString::Printf(TEXT("%s %s %s -project=\"%s\" -WaitMutex"),
					*TargetName, *Platform, *Configuration, *UProject);

				int32 ReturnCode = -1;
				FString StdOut;
				FString StdErr;
				const double Start = FPlatformTime::Seconds();
				const bool bLaunched = FPlatformProcess::ExecProcess(*Ubt, *Params, &ReturnCode, &StdOut, &StdErr);
				const double Elapsed = FPlatformTime::Seconds() - Start;
				if (!bLaunched)
				{
					return FUnrealOpenMcpToolDispatchResult::Fail(
						TEXT("ubt_launch_failed"),
						FString::Printf(TEXT("Failed to launch UnrealBuildTool '%s'."), *Ubt));
				}

				const FString Combined = StdOut + TEXT("\n") + StdErr;
				TArray<FSourceDiagnostic> Diagnostics;
				ParseDiagnostics(Combined, Diagnostics);

				int32 ErrorCount = 0;
				int32 WarningCount = 0;
				TArray<TSharedPtr<FJsonValue>> DiagJson;
				DiagJson.Reserve(Diagnostics.Num());
				for (const FSourceDiagnostic& Diag : Diagnostics)
				{
					if (Diag.Severity == TEXT("error"))
					{
						++ErrorCount;
					}
					else if (Diag.Severity == TEXT("warning"))
					{
						++WarningCount;
					}
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("file"), Diag.File);
					Entry->SetNumberField(TEXT("line"), Diag.Line);
					Entry->SetStringField(TEXT("severity"), Diag.Severity);
					Entry->SetStringField(TEXT("message"), Diag.Message);
					DiagJson.Add(MakeShared<FJsonValueObject>(Entry));
				}

				// success (process return 0) is split from compile_clean (zero
				// compiler errors): the editor holds the module DLL, so a relink
				// fails (success:false) even when the compile stage was clean
				// (compile_clean:true). Diagnostics are emitted before link.
				const bool bSuccess = (ReturnCode == 0);
				const bool bCompileClean = (ErrorCount == 0);

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("method"), TEXT("ubt"));
				Result->SetStringField(TEXT("target"), TargetName);
				Result->SetStringField(TEXT("configuration"), Configuration);
				Result->SetStringField(TEXT("platform"), Platform);
				Result->SetNumberField(TEXT("return_code"), ReturnCode);
				Result->SetBoolField(TEXT("success"), bSuccess);
				Result->SetBoolField(TEXT("compile_clean"), bCompileClean);
				Result->SetNumberField(TEXT("error_count"), ErrorCount);
				Result->SetNumberField(TEXT("warning_count"), WarningCount);
				Result->SetNumberField(TEXT("duration_seconds"), Elapsed);
				Result->SetArrayField(TEXT("diagnostics"), DiagJson);
				// A bounded tail of the raw output aids debugging when no
				// diagnostic matched (e.g. a link failure, or a UBT configuration
				// error) without flooding the response. 4000 chars mirrors the
				// Unreal-MCP behavior reference.
				Result->SetStringField(TEXT("output_tail"), Combined.Right(4000));
				return FUnrealOpenMcpToolDispatchResult::Ok(
					WriteJson(MakeShared<FJsonValueObject>(Result)));
			},
			FUnrealOpenMcpToolMetadata::Mutating());
	}
} // namespace FUnrealOpenMcpSourceTools
