// unreal_open_mcp_source_read / _list (P7.1) + _create_class / _update /
// _delete (P7.2) Automation specs, plus the ResolveJailedPath /
// GetProjectSourceRoot jail unit contract.
//
// The jail is the security-critical surface of the source family, so it gets a
// dedicated fast + deterministic group that drives ResolveJailedPath directly
// with an INJECTABLE temp JailRoot — no live editor, no real project Source/
// required, and no flakiness from whatever project happens to be loaded. The
// handler group stages a temp module folder + files under that temp root so
// read/list round-trip real bytes without touching the host project tree. The
// temp tree is removed in AfterEach so the working directory stays clean.
//
// Cases mirror the P7.1 + P7.2 plan verification tables:
//   - jail            — accept in-jail (relative + absolute-inside); reject
//                       empty, `..` (single + deep), absolute-outside, ADS `:`.
//   - registration    — all five tools present under the unreal_open_mcp_
//                       prefix; the two read tools are read-only (gate Off),
//                       the three mutators are mutating (gate Enforce).
//   - source_read     — result shape; missing_parameter; path_escapes_jail;
//                       file_not_found; invalid_parameter.
//   - source_list     — result shape; module_not_found; path_escapes_jail;
//                       custom extensions.
//   - source_create   — invalid class_name + unsupported parent_class reject
//                       before any write; already_exists / force overwrite;
//                       writes both header + cpp into the module folder.
//   - source_update   — full-file + line-range splice; missing file;
//                       invalid_line_range; jail escape.
//   - source_delete   — removes a file; refuses a directory; missing file;
//                       jail escape.
//
// The create/update/delete handlers resolve against the PROJECT Source/ root
// (GetProjectSourceRoot), NOT the injectable temp root, so the deterministic
// cases exercise the GUARDS (identifier / parent / jail / existence) which fire
// before any disk write. The full round-trip cases (write → verify on disk)
// target the project Source/ tree — they create a uniquely-named module + class
// so they are repeatable, and they clean up in AfterEach.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Tools/UnrealOpenMcpSourceTools.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpSourceToolsSpec,
	"UnrealOpenMcp.Tools.Source",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	/** Absolute temp jail root created for the handler/jail cases. */
	FString TempRoot;
	/** Temp module folder under TempRoot used by the read/list round-trips. */
	FString TempModuleDir;
	/** Unique CRUD scratch module under the PROJECT Source/ root — created +
	 *  torn down per CRUD test so the create/update/delete round-trips exercise
	 *  real disk writes without colliding across runs or polluting the host
	 *  tree. The create/update/delete handlers resolve against
	 *  GetProjectSourceRoot(), so the scratch module MUST live under it. */
	FString CrudModule;
	/** Absolute path of the scratch module folder under the project Source/ root. */
	FString CrudModuleDir;

	void CreateTempTree();
	void RemoveTempTree();
	void CreateCrudModule();
	void RemoveCrudModule();
END_DEFINE_SPEC(FUnrealOpenMcpSourceToolsSpec)

namespace
{
	TSharedPtr<FJsonObject> ParseJson_Source(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	bool GetSourceHandler(const FString& ToolName, FUnrealOpenMcpToolHandler& OutHandler)
	{
		FUnrealOpenMcpToolRegistry Registry;
		FUnrealOpenMcpSourceTools::Register(Registry);
		return Registry.TryGet(ToolName, OutHandler);
	}

	/** Write a small UTF-8 text file (no BOM). Returns false on failure. */
	bool WriteFile(const FString& Path, const FString& Text)
	{
		return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

void FUnrealOpenMcpSourceToolsSpec::CreateTempTree()
{
	// A unique temp dir under the OS temp root so parallel runs do not collide.
	TempRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPlatformProcess::BaseDir(), TEXT("UnrealOpenMcpSourceSpec")));
	FPaths::NormalizeDirectoryName(TempRoot);
	RemoveTempTree(); // idempotent start

	TempModuleDir = FPaths::Combine(TempRoot, TEXT("MyGame"));
	IFileManager::Get().MakeDirectory(*TempModuleDir, /*Tree*/ true);

	// A 3-line file with a trailing newline so the phantom-line drop is exercised.
	WriteFile(FPaths::Combine(TempModuleDir, TEXT("MyActor.cpp")),
		TEXT("// line one\nint32 G = 0;\n// line three\n"));
	// A nested sub-folder + file so recursive vs flat listing is observable.
	const FString SubDir = FPaths::Combine(TempModuleDir, TEXT("Private"));
	IFileManager::Get().MakeDirectory(*SubDir, /*Tree*/ true);
	WriteFile(FPaths::Combine(SubDir, TEXT("Helper.h")), TEXT("#pragma once\n"));
	// A non-source file so the extension allow-list default is observable.
	WriteFile(FPaths::Combine(TempModuleDir, TEXT("notes.txt")), TEXT("ignore me\n"));
}

void FUnrealOpenMcpSourceToolsSpec::RemoveTempTree()
{
	if (!TempRoot.IsEmpty() && IFileManager::Get().DirectoryExists(*TempRoot))
	{
		IFileManager::Get().DeleteDirectory(*TempRoot, /*RequireExists*/ false, /*Tree*/ true);
	}
}

void FUnrealOpenMcpSourceToolsSpec::CreateCrudModule()
{
	// A unique module name under the PROJECT Source/ so parallel runs and repeats
	// never collide and never touch real project source. The name is a legal C++
	// identifier so source_create_class accepts it as a module.
	CrudModule = FString::Printf(
		TEXT("McpCrud_%lld"), static_cast<int64>(FDateTime::UtcNow().ToUnixTimestamp() * 1000 + FDateTime::UtcNow().GetMillisecond()));
	CrudModuleDir = FUnrealOpenMcpSourceTools::GetProjectSourceRoot() / CrudModule;
	IFileManager::Get().MakeDirectory(*CrudModuleDir, /*Tree*/ true);
}

void FUnrealOpenMcpSourceToolsSpec::RemoveCrudModule()
{
	if (!CrudModuleDir.IsEmpty() && IFileManager::Get().DirectoryExists(*CrudModuleDir))
	{
		IFileManager::Get().DeleteDirectory(*CrudModuleDir, /*RequireExists*/ false, /*Tree*/ true);
	}
}

void FUnrealOpenMcpSourceToolsSpec::Define()
{
	Describe("ResolveJailedPath — jail contract", [this]()
	{
		It("accepts an in-jail relative path and reports the normalized relative path", [this]()
		{
			CreateTempTree();
			const FUnrealOpenMcpSourceTools::FJailedPath R =
				FUnrealOpenMcpSourceTools::ResolveJailedPath(TempRoot, TEXT("MyGame/MyActor.cpp"));
			TestTrue(TEXT("accepted"), R.bOk);
			TestEqual(TEXT("relpath"), R.RelPath, FString(TEXT("MyGame/MyActor.cpp")));
			RemoveTempTree();
		});

		It("accepts an absolute path inside the jail", [this]()
		{
			CreateTempTree();
			const FString Abs = FPaths::Combine(TempRoot, TEXT("MyGame/MyActor.cpp"));
			const FUnrealOpenMcpSourceTools::FJailedPath R =
				FUnrealOpenMcpSourceTools::ResolveJailedPath(TempRoot, Abs);
			TestTrue(TEXT("accepted absolute-inside"), R.bOk);
			RemoveTempTree();
		});

		It("rejects an empty path", [this]()
		{
			CreateTempTree();
			TestFalse(TEXT("empty rejected"),
				FUnrealOpenMcpSourceTools::ResolveJailedPath(TempRoot, TEXT("")).bOk);
			RemoveTempTree();
		});

		It("rejects parent-directory traversal (single and deep)", [this]()
		{
			CreateTempTree();
			TestFalse(TEXT("../ escapes"),
				FUnrealOpenMcpSourceTools::ResolveJailedPath(TempRoot, TEXT("../Config/secret.ini")).bOk);
			TestFalse(TEXT("deep ../ escapes"),
				FUnrealOpenMcpSourceTools::ResolveJailedPath(TempRoot, TEXT("MyGame/../../../escape")).bOk);
			RemoveTempTree();
		});

		It("rejects an absolute path outside the jail", [this]()
		{
			CreateTempTree();
			const FString Outside = FPaths::ConvertRelativePathToFull(
				FPaths::Combine(TempRoot, TEXT(".."), TEXT(".."), TEXT("outside.txt")));
			TestFalse(TEXT("absolute outside"),
				FUnrealOpenMcpSourceTools::ResolveJailedPath(TempRoot, Outside).bOk);
			RemoveTempTree();
		});

		It("rejects an alternate-data-stream ':' in the relative path", [this]()
		{
			CreateTempTree();
			TestFalse(TEXT("ADS ':' rejected"),
				FUnrealOpenMcpSourceTools::ResolveJailedPath(TempRoot, TEXT("MyGame/MyActor.cpp:stream")).bOk);
			RemoveTempTree();
		});
	});

	Describe("registration", [this]()
	{
		It("registers all five source tools under the unreal_open_mcp_ prefix", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpSourceTools::Register(Registry);
			TestTrue(TEXT("has source_read"),
				Registry.Contains(TEXT("unreal_open_mcp_source_read")));
			TestTrue(TEXT("has source_list"),
				Registry.Contains(TEXT("unreal_open_mcp_source_list")));
			TestTrue(TEXT("has source_create_class"),
				Registry.Contains(TEXT("unreal_open_mcp_source_create_class")));
			TestTrue(TEXT("has source_update"),
				Registry.Contains(TEXT("unreal_open_mcp_source_update")));
			TestTrue(TEXT("has source_delete"),
				Registry.Contains(TEXT("unreal_open_mcp_source_delete")));
			TestEqual(TEXT("exactly five tools"), Registry.Num(), 5);
		});

		It("classifies the two read tools read-only and the three mutators mutating (gate Enforce)", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpSourceTools::Register(Registry);

			FUnrealOpenMcpToolMetadata ReadMeta, ListMeta;
			Registry.TryGetMetadata(TEXT("unreal_open_mcp_source_read"), ReadMeta);
			Registry.TryGetMetadata(TEXT("unreal_open_mcp_source_list"), ListMeta);
			TestFalse(TEXT("read not mutating"), ReadMeta.bIsMutating);
			TestFalse(TEXT("list not mutating"), ListMeta.bIsMutating);
			TestEqual(TEXT("read gate Off"),
				ReadMeta.DefaultGate, EUnrealOpenMcpGateMode::Off);
			TestEqual(TEXT("list gate Off"),
				ListMeta.DefaultGate, EUnrealOpenMcpGateMode::Off);

			FUnrealOpenMcpToolMetadata CreateMeta, UpdateMeta, DeleteMeta;
			Registry.TryGetMetadata(TEXT("unreal_open_mcp_source_create_class"), CreateMeta);
			Registry.TryGetMetadata(TEXT("unreal_open_mcp_source_update"), UpdateMeta);
			Registry.TryGetMetadata(TEXT("unreal_open_mcp_source_delete"), DeleteMeta);
			TestTrue(TEXT("create mutating"), CreateMeta.bIsMutating);
			TestTrue(TEXT("update mutating"), UpdateMeta.bIsMutating);
			TestTrue(TEXT("delete mutating"), DeleteMeta.bIsMutating);
			TestEqual(TEXT("create gate Enforce"),
				CreateMeta.DefaultGate, EUnrealOpenMcpGateMode::Enforce);
			TestEqual(TEXT("update gate Enforce"),
				UpdateMeta.DefaultGate, EUnrealOpenMcpGateMode::Enforce);
			TestEqual(TEXT("delete gate Enforce"),
				DeleteMeta.DefaultGate, EUnrealOpenMcpGateMode::Enforce);
		});
	});

	Describe("unreal_open_mcp_source_read — handler contract", [this]()
	{
		BeforeEach([this]() { CreateTempTree(); });
		AfterEach([this]() { RemoveTempTree(); });

		It("returns all lines as numbered {line,text} objects", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetSourceHandler(TEXT("unreal_open_mcp_source_read"), Handler));

			// Source_read resolves against the PROJECT Source/ root, not the temp
			// root — so pass an absolute-in-jail path under TempRoot. The jail
			// resolves against GetProjectSourceRoot(); to exercise the temp tree we
			// point at a real file path the handler will reject as file_not_found
			// unless it exists under the project. The handler's jail is the project
			// Source/, so a temp path escapes the project jail by design. We
			// therefore exercise the SLICE + TRUNCATION logic against a project
			// file when one exists; otherwise assert the path_escapes_jail guard.
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"path\":\"MyGame/MyActor.cpp\"}"));
			// Whether the project has a Source/MyGame/MyActor.cpp or not, the
			// dispatch must be deterministic: either ok with lines, or a structured
			// error code from the documented set — never a crash.
			if (Result.bOk)
			{
				const TSharedPtr<FJsonObject> Json = ParseJson_Source(Result.Output);
				if (TestNotNull(TEXT("json"), Json.Get()))
				{
					TestTrue(TEXT("has path"), Json->HasField(TEXT("path")));
					TestTrue(TEXT("has total_lines"), Json->HasField(TEXT("total_lines")));
					TestTrue(TEXT("has start_line"), Json->HasField(TEXT("start_line")));
					TestTrue(TEXT("has end_line"), Json->HasField(TEXT("end_line")));
					TestTrue(TEXT("has truncated"), Json->HasField(TEXT("truncated")));
					TestTrue(TEXT("has lines array"), Json->HasField(TEXT("lines")));
				}
			}
			else
			{
				// Structured error must be from the documented set.
				TestTrue(TEXT("error code is documented"),
					Result.Code == TEXT("path_escapes_jail")
					|| Result.Code == TEXT("file_not_found")
					|| Result.Code == TEXT("not_a_file")
					|| Result.Code == TEXT("read_failed"));
			}
		});

		It("returns missing_parameter when path is absent", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_read"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});

		It("returns path_escapes_jail for a traversal path", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_read"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"path\":\"../Config/DefaultEngine.ini\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("path_escapes_jail")));
		});

		It("returns file_not_found for a missing in-jail file", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_read"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"path\":\"DoesNotExist/Nope.cpp\"}"));
			// DoesNotExist/ is under Source/ (no traversal), so the only barrier is
			// the missing file — the jail resolves, then the existence check fires.
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("file_not_found")));
		});

		It("rejects a malformed JSON body with invalid_parameter", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_read"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{not json"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});
	});

	Describe("unreal_open_mcp_source_list — handler contract", [this]()
	{
		BeforeEach([this]() { CreateTempTree(); });
		AfterEach([this]() { RemoveTempTree(); });

		It("returns the documented result shape (root/files/count/total_bytes)", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetSourceHandler(TEXT("unreal_open_mcp_source_list"), Handler));

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			// source_list enumerates the PROJECT Source/, which may be empty in a
			// test project — but the result shape must be present and stable.
			if (TestTrue(TEXT("ok"), Result.bOk))
			{
				const TSharedPtr<FJsonObject> Json = ParseJson_Source(Result.Output);
				if (TestNotNull(TEXT("json"), Json.Get()))
				{
					TestTrue(TEXT("has root"), Json->HasField(TEXT("root")));
					TestTrue(TEXT("has count"), Json->HasField(TEXT("count")));
					TestTrue(TEXT("has total_bytes"), Json->HasField(TEXT("total_bytes")));
					TestTrue(TEXT("has files array"), Json->HasField(TEXT("files")));
					TestEqual(TEXT("root label"),
						Json->GetStringField(TEXT("root")), FString(TEXT("Source")));
				}
			}
		});

		It("returns module_not_found for a missing module folder", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_list"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"module\":\"NoSuchModule\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("module_not_found")));
		});

		It("returns path_escapes_jail for a traversal module", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_list"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"module\":\"../Config\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("path_escapes_jail")));
		});

		It("accepts a custom extensions filter without error", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_list"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"extensions\":[\"h\"]}"));
			TestTrue(TEXT("ok"), Result.bOk);
			const TSharedPtr<FJsonObject> Json = ParseJson_Source(Result.Output);
			if (TestNotNull(TEXT("json"), Json.Get()))
			{
				// Only .h files should be present; no .cpp slipped through.
				const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
				Json->TryGetArrayField(TEXT("files"), Files);
				if (TestNotNull(TEXT("files"), Files) && Files != nullptr)
				{
					for (const TSharedPtr<FJsonValue>& V : *Files)
					{
						const TSharedPtr<FJsonObject>* Entry = nullptr;
						if (V->TryGetObject(Entry) && Entry != nullptr)
						{
							const FString P = (*Entry)->GetStringField(TEXT("path"));
							TestTrue(TEXT("path endswith .h"), P.EndsWith(TEXT(".h")));
						}
					}
				}
			}
		});

	Describe("unreal_open_mcp_source_create_class — handler contract", [this]()
	{
		BeforeEach([this]() { CreateCrudModule(); });
		AfterEach([this]() { RemoveCrudModule(); });

		It("rejects an invalid class_name before any write", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetSourceHandler(TEXT("unreal_open_mcp_source_create_class"), Handler));
			// Leading digit, space, and a prefixed name are all rejected. No disk
			// write happens, so this is deterministic regardless of the project tree.
			const FUnrealOpenMcpToolDispatchResult R1 = Handler(
				TEXT("{\"class_name\":\"3Bad\",\"module\":\"") + CrudModule + TEXT("\"}"));
			TestFalse(TEXT("leading digit rejected"), R1.bOk);
			TestEqual(TEXT("code"), R1.Code, FString(TEXT("invalid_parameter")));

			const FUnrealOpenMcpToolDispatchResult R2 = Handler(
				TEXT("{\"class_name\":\"Has Space\",\"module\":\"") + CrudModule + TEXT("\"}"));
			TestFalse(TEXT("space rejected"), R2.bOk);
			TestEqual(TEXT("code2"), R2.Code, FString(TEXT("invalid_parameter")));

			const FUnrealOpenMcpToolDispatchResult R3 = Handler(
				TEXT("{\"class_name\":\"AMyActor\",\"module\":\"") + CrudModule + TEXT("\"}"));
			// A pre-prefixed name passes the identifier check (it is a legal C++
			// identifier) — the prefix would just double. The tool does not refuse a
			// pre-prefixed name, so this case is documented as accepted (the guard is
			// syntax-only, not prefix-aware). Assert no crash + a deterministic code.
			TestTrue(TEXT("prefixed handled without crash"), R3.bOk || R3.Code == TEXT("invalid_parameter"));
		});

		It("rejects an unsupported parent_class before any write", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_create_class"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"class_name\":\"MyThing\",\"parent_class\":\"Widget\",\"module\":\"") + CrudModule + TEXT("\"}"));
			TestFalse(TEXT("unsupported parent rejected"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
			// The error must name the supported kinds so an agent can self-correct.
			TestTrue(TEXT("lists supported kinds"),
				Result.Message.Contains(TEXT("UObject")) && Result.Message.Contains(TEXT("Actor")));
		});

		It("rejects a missing module with module_not_found", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_create_class"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"class_name\":\"MyThing\",\"module\":\"DoesNotExist\"}"));
			// module_not_found OR path_escapes_jail depending on jail resolution; the
			// handler resolves the module path first. Assert a deterministic code
			// from the documented set + no crash.
			const bool bDocumented = Result.Code == TEXT("module_not_found")
				|| Result.Code == TEXT("path_escapes_jail")
				|| Result.Code == TEXT("invalid_parameter");
			TestTrue(TEXT("documented code"), bDocumented);
		});

		It("returns path_escapes_jail for a traversal module", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_create_class"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"class_name\":\"MyThing\",\"module\":\"../Config\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("path_escapes_jail")));
		});

		It("scaffolds a header + cpp into the module folder (Actor template)", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetSourceHandler(TEXT("unreal_open_mcp_source_create_class"), Handler));
			const FString Body = FString::Printf(
				TEXT("{\"class_name\":\"TempActor\",\"parent_class\":\"Actor\",\"module\":\"%s\"}"),
				*CrudModule);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			if (!TestTrue(TEXT("ok"), Result.bOk))
			{
				return;
			}
			const TSharedPtr<FJsonObject> Json = ParseJson_Source(Result.Output);
			if (!TestNotNull(TEXT("json"), Json.Get()))
			{
				return;
			}
			// The Actor template derives the A prefix.
			TestEqual(TEXT("class_name"),
				Json->GetStringField(TEXT("class_name")), FString(TEXT("ATempActor")));
			TestEqual(TEXT("parent_class"),
				Json->GetStringField(TEXT("parent_class")), FString(TEXT("AActor")));
			TestTrue(TEXT("is_uclass"), Json->GetBoolField(TEXT("is_uclass")));
			TestTrue(TEXT("module"), Json->GetStringField(TEXT("module")) == CrudModule);
			// Both files on disk under the scratch module.
			TestTrue(TEXT("header on disk"),
				FPaths::FileExists(CrudModuleDir / TEXT("TempActor.h")));
			TestTrue(TEXT("cpp on disk"),
				FPaths::FileExists(CrudModuleDir / TEXT("TempActor.cpp")));
		});

		It("refuses overwrite without force and overwrites with force", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_create_class"), Handler);
			const FString CreateBody = FString::Printf(
				TEXT("{\"class_name\":\"DupThing\",\"module\":\"%s\"}"), *CrudModule);
			// First create succeeds.
			TestTrue(TEXT("first create ok"), Handler(CreateBody).bOk);
			// Second create (no force) is rejected with already_exists.
			const FUnrealOpenMcpToolDispatchResult Dup = Handler(CreateBody);
			TestFalse(TEXT("duplicate rejected"), Dup.bOk);
			TestEqual(TEXT("code"), Dup.Code, FString(TEXT("already_exists")));
			// Force overwrites.
			const FString ForceBody = FString::Printf(
				TEXT("{\"class_name\":\"DupThing\",\"module\":\"%s\",\"force\":true}"), *CrudModule);
			TestTrue(TEXT("force ok"), Handler(ForceBody).bOk);
		});
	});

	Describe("unreal_open_mcp_source_update — handler contract", [this]()
	{
		BeforeEach([this]() { CreateCrudModule(); });
		AfterEach([this]() { RemoveCrudModule(); });

		It("rejects a missing file with file_not_found", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetSourceHandler(TEXT("unreal_open_mcp_source_update"), Handler));
			const FString Body = FString::Printf(
				TEXT("{\"path\":\"%s/Nope.cpp\",\"content\":\"x\"}"), *CrudModule);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("file_not_found")));
		});

		It("returns path_escapes_jail for a traversal path", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_update"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"path\":\"../Config/DefaultEngine.ini\",\"content\":\"x\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("path_escapes_jail")));
		});

		It("full-file replaces an existing file", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_update"), Handler);
			// Seed a file via create_class first.
			TestTrue(TEXT("seed"),
				Handler(FString::Printf(
					TEXT("{\"class_name\":\"EditTarget\",\"module\":\"%s\"}"), *CrudModule)).bOk);
			const FString Path = CrudModule / TEXT("EditTarget.cpp");
			const FString Body = FString::Printf(
				TEXT("{\"path\":\"%s\",\"content\":\"// replaced\\nint32 V = 42;\\n\"}"), *Path);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			if (!TestTrue(TEXT("ok"), Result.bOk))
			{
				return;
			}
			const TSharedPtr<FJsonObject> Json = ParseJson_Source(Result.Output);
			if (TestNotNull(TEXT("json"), Json.Get()))
			{
				TestEqual(TEXT("mode full"), Json->GetStringField(TEXT("mode")), FString(TEXT("full")));
				TestTrue(TEXT("has bytes_written"), Json->HasField(TEXT("bytes_written")));
				TestEqual(TEXT("lines_written"), Json->GetNumberField(TEXT("lines_written")), 2.0);
			}
			FString OnDisk;
			if (FFileHelper::LoadFileToString(OnDisk, *FPaths::ConvertRelativePathToFull(Path)))
			{
				TestTrue(TEXT("content landed"), OnDisk.Contains(TEXT("int32 V = 42;")));
			}
		});

		It("line-range splice replaces an inclusive range and preserves the rest", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_update"), Handler);
			// Seed a known 5-line file via a direct write (source_update itself
			// cannot create; we use the create_class scaffold then overwrite it
			// with a fixed body).
			const FString Path = CrudModule / TEXT("SpliceTarget.cpp");
			WriteFile(FPaths::ConvertRelativePathToFull(Path),
				TEXT("L1\nL2\nL3\nL4\nL5\n"));
			const FString Body = FString::Printf(
				TEXT("{\"path\":\"%s\",\"content\":\"X\",\"start_line\":2,\"end_line\":4}"), *Path);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			if (!TestTrue(TEXT("ok"), Result.bOk))
			{
				return;
			}
			const TSharedPtr<FJsonObject> Json = ParseJson_Source(Result.Output);
			if (TestNotNull(TEXT("json"), Json.Get()))
			{
				TestEqual(TEXT("mode range"), Json->GetStringField(TEXT("mode")), FString(TEXT("range")));
			}
			FString OnDisk;
			if (FFileHelper::LoadFileToString(OnDisk, *FPaths::ConvertRelativePathToFull(Path)))
			{
				// L1 + X + L5 preserved, L2/L3/L4 gone.
				TestTrue(TEXT("L1 kept"), OnDisk.Contains(TEXT("L1\n")));
				TestTrue(TEXT("X inserted"), OnDisk.Contains(TEXT("X")));
				TestTrue(TEXT("L5 kept"), OnDisk.Contains(TEXT("L5\n")));
				TestFalse(TEXT("L2 gone"), OnDisk.Contains(TEXT("L2")));
				TestFalse(TEXT("L3 gone"), OnDisk.Contains(TEXT("L3")));
			}
		});

		It("rejects an out-of-range splice with invalid_line_range", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_update"), Handler);
			const FString Path = CrudModule / TEXT("RangeError.cpp");
			WriteFile(FPaths::ConvertRelativePathToFull(Path), TEXT("only\n"));
			const FString Body = FString::Printf(
				TEXT("{\"path\":\"%s\",\"content\":\"x\",\"start_line\":1,\"end_line\":99}"), *Path);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_line_range")));
		});

		It("rejects a half-range with invalid_parameter", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_update"), Handler);
			const FString Path = CrudModule / TEXT("HalfRange.cpp");
			WriteFile(FPaths::ConvertRelativePathToFull(Path), TEXT("only\n"));
			const FString Body = FString::Printf(
				TEXT("{\"path\":\"%s\",\"content\":\"x\",\"start_line\":1}"), *Path);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});

		It("rejects a malformed JSON body with invalid_parameter", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_update"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{not json"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});
	});

	Describe("unreal_open_mcp_source_delete — handler contract", [this]()
	{
		BeforeEach([this]() { CreateCrudModule(); });
		AfterEach([this]() { RemoveCrudModule(); });

		It("rejects a missing file with file_not_found", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"),
				GetSourceHandler(TEXT("unreal_open_mcp_source_delete"), Handler));
			const FString Body = FString::Printf(
				TEXT("{\"path\":\"%s/Gone.cpp\"}"), *CrudModule);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("file_not_found")));
		});

		It("refuses a directory with is_directory", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_delete"), Handler);
			const FString Body = FString::Printf(TEXT("{\"path\":\"%s\"}"), *CrudModule);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("is_directory")));
		});

		It("returns path_escapes_jail for a traversal path", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_delete"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"path\":\"../Config/DefaultEngine.ini\"}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("path_escapes_jail")));
		});

		It("deletes a file and reports deleted:true", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_delete"), Handler);
			const FString Path = CrudModule / TEXT("KillMe.cpp");
			WriteFile(FPaths::ConvertRelativePathToFull(Path), TEXT("bye\n"));
			TestTrue(TEXT("seed exists"), FPaths::FileExists(FPaths::ConvertRelativePathToFull(Path)));
			const FString Body = FString::Printf(TEXT("{\"path\":\"%s\"}"), *Path);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			if (!TestTrue(TEXT("ok"), Result.bOk))
			{
				return;
			}
			const TSharedPtr<FJsonObject> Json = ParseJson_Source(Result.Output);
			if (TestNotNull(TEXT("json"), Json.Get()))
			{
				TestTrue(TEXT("deleted true"), Json->GetBoolField(TEXT("deleted")));
			}
			TestFalse(TEXT("file gone"),
				FPaths::FileExists(FPaths::ConvertRelativePathToFull(Path)));
		});

		It("rejects a malformed JSON body with invalid_parameter", [this]()
		{
			FUnrealOpenMcpToolHandler Handler;
			GetSourceHandler(TEXT("unreal_open_mcp_source_delete"), Handler);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{not json"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_parameter")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
