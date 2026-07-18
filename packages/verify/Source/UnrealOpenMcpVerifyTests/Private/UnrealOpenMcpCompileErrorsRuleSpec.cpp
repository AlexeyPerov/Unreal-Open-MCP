// FCompileErrorsRule Automation specs (P3.4).
//
// Adapts the MissingBlueprintParent spec shape (P3.3) to the compile_errors
// rule. The spec cannot easily author a real broken C++ build in CI (a
// broken build cannot be cleanly recovered from a headless test runner), so
// it injects a fake ICompileStatusProvider that returns a controlled
// ECompileState + TArray<FCompileDiagnostic> per test. The fake decouples
// the spec from ILiveCodingModule / the message log so cases are fully
// deterministic.
//
// Pins (acceptance criteria from specs/execution/P3/P3.4.md):
//   - Active compile failures emit compile_error Errors with stable evidence
//     fields (file, line, column?, message, module?, source).
//   - Clean compile state produces no issues.
//   - A compile in flight (InProgressOrUnknown) produces no issues — a
//     finding emitted mid-compile would be stale before it reached the agent.
//   - The rule does not invoke any compile-triggering API (the fake tracks
//     call counts; the rule never asks the provider to compile).
//   - A Failed state with no per-file diagnostics emits a single coarse
//     "(project)" finding (a known failure is never silently swallowed).
//   - A non-empty paths_hint filters per-file findings by File prefix while
//     keeping the coarse "(project)" finding visible.
//   - The rule is registered by RegisterDefaults with the stable Id.
//   - Every emitted issue has a parseable FIssueKey and the right evidence.
//   - The issue integrates with the fix-matching contract (ruleId + bare
//     issueCode).
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Core/IssueKey.h"
#include "Core/IVerifyRule.h"
#include "Core/VerifyIssue.h"
#include "Core/VerifyRunMode.h"
#include "Core/VerifyRunner.h"
#include "Core/VerifyScope.h"
#include "Core/VerifySeverity.h"
#include "Rules/CompileErrors/CompileErrorsIssueCodes.h"
#include "Rules/CompileErrors/CompileErrorsRule.h"
#include "Rules/CompileErrors/ICompileStatusProvider.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpCompileErrorsRuleSpec,
	"UnrealOpenMcp.Verify.CompileErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpCompileErrorsRuleSpec)

namespace
{

// Fake provider: holds a controlled ECompileState + diagnostics list, and
// counts how many times each method is invoked so the spec can pin the
// "no compile side effects" contract (the rule must never ask the provider
// to compile; GetState / GetDiagnostics are read-only by construction).
class FFakeCompileStatusProvider final : public ICompileStatusProvider
{
public:
	ECompileState State = ECompileState::Clean;
	TArray<FCompileDiagnostic> Diagnostics;

	mutable int32 GetStateCallCount = 0;
	mutable int32 GetDiagnosticsCallCount = 0;

	virtual ECompileState GetState() const override
	{
		++GetStateCallCount;
		return State;
	}

	virtual void GetDiagnostics(TArray<FCompileDiagnostic>& OutDiagnostics) const override
	{
		++GetDiagnosticsCallCount;
		OutDiagnostics = Diagnostics;
	}
};

// Build a structured diagnostic with the given fields. FCompileDiagnostic
// lives at global scope alongside ICompileStatusProvider (mirrors
// EBlueprintParentResolution next to IBlueprintParentResolver).
FCompileDiagnostic MakeDiagnostic(
	const FString& File, int32 Line, int32 Column, const FString& Message, const FString& Module)
{
	FCompileDiagnostic D;
	D.File = File;
	D.Line = Line;
	D.Column = Column;
	D.Message = Message;
	D.Module = Module;
	return D;
}

// RAII guard mirroring FScopedRules in the runner spec: clear the rule
// registry on construct / destruct so each spec starts and ends clean.
struct FScopedRules
{
	FScopedRules() { FVerifyRunner::ClearRules(); }
	~FScopedRules() { FVerifyRunner::ClearRules(); }
};

} // namespace

void FUnrealOpenMcpCompileErrorsRuleSpec::Define()
{
	Describe("RegisterDefaults", [this]()
	{
		It("registers compile_errors with the stable Id", [this]()
		{
			FScopedRules Guard;
			FVerifyRunner::RegisterDefaults();

			const auto& Rules = FVerifyRunner::GetRules();
			bool bFound = false;
			for (const TUniquePtr<IVerifyRule>& Rule : Rules)
			{
				if (Rule.IsValid() && Rule->GetId() == UnrealOpenMcpVerify::CompileErrors::RuleId)
				{
					bFound = true;
					break;
				}
			}
			TestTrue(TEXT("rule registered"), bFound);
		});

		It("does not double-register on repeated calls", [this]()
		{
			FScopedRules Guard;
			FVerifyRunner::RegisterDefaults();
			FVerifyRunner::RegisterDefaults();

			int32 Count = 0;
			for (const TUniquePtr<IVerifyRule>& Rule : FVerifyRunner::GetRules())
			{
				if (Rule.IsValid() && Rule->GetId() == UnrealOpenMcpVerify::CompileErrors::RuleId)
				{
					++Count;
				}
			}
			TestEqual(TEXT("single registration"), Count, 1);
		});
	});

	Describe("FCompileErrorsRule::GetId", [this]()
	{
		It("returns the stable rule Id", [this]()
		{
			auto Provider = MakeUnique<FFakeCompileStatusProvider>();
			FCompileErrorsRule Rule(MoveTemp(Provider));
			TestEqual(TEXT("id"), Rule.GetId(), FString(UnrealOpenMcpVerify::CompileErrors::RuleId));
		});
	});

	Describe("Scan - clean state", [this]()
	{
		It("emits no issues when the provider reports Clean", [this]()
		{
			auto Provider = MakeUnique<FFakeCompileStatusProvider>();
			Provider->State = ECompileState::Clean;
			FCompileErrorsRule Rule(MoveTemp(Provider));

			TArray<FString> Paths;
			Paths.Add(TEXT("Source/MyModule/Foo.cpp"));
			FVerifyScope Scope(MoveTemp(Paths));

			TArray<FVerifyIssue> Issues;
			Rule.Scan(Scope, EVerifyRunMode::Full, Issues);

			TestEqual(TEXT("no issues for clean state"), Issues.Num(), 0);
		});
	});

	Describe("Scan - compile in flight", [this]()
	{
		It("emits no issues when the provider reports InProgressOrUnknown", [this]()
		{
			auto Provider = MakeUnique<FFakeCompileStatusProvider>();
			Provider->State = ECompileState::InProgressOrUnknown;
			// Even if the provider claims it has diagnostics, the rule must
			// ignore them while the compile is in flight — a finding emitted
			// now would be stale before it reached the agent.
			Provider->Diagnostics.Add(MakeDiagnostic(
				TEXT("Source/MyModule/Foo.cpp"), 42, 5,
				TEXT("expected ';'"), TEXT("MyModule")));

			FCompileErrorsRule Rule(MoveTemp(Provider));

			TArray<FString> Paths;
			Paths.Add(TEXT("Source/MyModule/Foo.cpp"));
			FVerifyScope Scope(MoveTemp(Paths));

			TArray<FVerifyIssue> Issues;
			Rule.Scan(Scope, EVerifyRunMode::Full, Issues);

			TestEqual(TEXT("no issues while compile in flight"), Issues.Num(), 0);
		});
	});

	Describe("Scan - failed with per-file diagnostics", [this]()
	{
		It("emits one compile_error Error per diagnostic on a Full (empty scope) scan", [this]()
		{
			auto Provider = MakeUnique<FFakeCompileStatusProvider>();
			Provider->State = ECompileState::Failed;
			Provider->Diagnostics.Add(MakeDiagnostic(
				TEXT("Source/MyModule/Foo.cpp"), 42, 5,
				TEXT("'Foo': undeclared identifier"), TEXT("MyModule")));
			Provider->Diagnostics.Add(MakeDiagnostic(
				TEXT("Source/MyModule/Bar.cpp"), 17, 0,
				TEXT("missing ';' before '}'"), TEXT("")));

			FCompileErrorsRule Rule(MoveTemp(Provider));

			FVerifyScope Scope; // empty paths_hint = whole-project Full scan

			TArray<FVerifyIssue> Issues;
			Rule.Scan(Scope, EVerifyRunMode::Full, Issues);

			TestEqual(TEXT("two issues"), Issues.Num(), 2);
			if (Issues.Num() != 2)
			{
				return;
			}

			for (const FVerifyIssue& Issue : Issues)
			{
				TestEqual(TEXT("ruleId"), Issue.RuleId, FString(UnrealOpenMcpVerify::CompileErrors::RuleId));
				TestEqual(TEXT("severity"), Issue.Severity, EVerifySeverity::Error);
				TestTrue(TEXT("issueCode starts with bare code"),
					Issue.IssueCode.StartsWith(UnrealOpenMcpVerify::CompileErrors::IssueCode));
				TestTrue(TEXT("evidence has file"), Issue.Evidence.Contains(TEXT("file")));
				TestTrue(TEXT("evidence has line"), Issue.Evidence.Contains(TEXT("line")));
				TestTrue(TEXT("evidence has column"), Issue.Evidence.Contains(TEXT("column")));
				TestTrue(TEXT("evidence has message"), Issue.Evidence.Contains(TEXT("message")));
				TestTrue(TEXT("evidence has module"), Issue.Evidence.Contains(TEXT("module")));
				TestTrue(TEXT("evidence has source"),
					Issue.Evidence.Contains(TEXT("source")));
				TestEqual(TEXT("source = structured"),
					Issue.Evidence[TEXT("source")], FString(TEXT("structured")));
			}

			// First diagnostic carries non-empty file + line, so its issue
			// key's AssetPath is the file path and its suffix is "<file>:<line>".
			const FVerifyIssue& First = Issues[0];
			TestEqual(TEXT("first AssetPath"), First.AssetPath, FString(TEXT("Source/MyModule/Foo.cpp")));
			TestEqual(TEXT("first evidence file"), First.Evidence[TEXT("file")], FString(TEXT("Source/MyModule/Foo.cpp")));
			TestEqual(TEXT("first evidence line"), First.Evidence[TEXT("line")], FString::FromInt(42));
			TestEqual(TEXT("first evidence column"), First.Evidence[TEXT("column")], FString::FromInt(5));
			TestEqual(TEXT("first evidence message"), First.Evidence[TEXT("message")], FString(TEXT("'Foo': undeclared identifier")));
			TestEqual(TEXT("first evidence module"), First.Evidence[TEXT("module")], FString(TEXT("MyModule")));
		});

		It("produces a parseable FIssueKey for every finding", [this]()
		{
			auto Provider = MakeUnique<FFakeCompileStatusProvider>();
			Provider->State = ECompileState::Failed;
			Provider->Diagnostics.Add(MakeDiagnostic(
				TEXT("Source/MyModule/Foo.cpp"), 42, 5,
				TEXT("expected ';'"), TEXT("")));

			FCompileErrorsRule Rule(MoveTemp(Provider));

			FVerifyScope Scope;

			TArray<FVerifyIssue> Issues;
			Rule.Scan(Scope, EVerifyRunMode::Full, Issues);

			TestEqual(TEXT("one issue"), Issues.Num(), 1);
			if (Issues.Num() != 1)
			{
				return;
			}

			const FString Key = FIssueKey::Build(Issues[0]);
			TestTrue(
				FString::Printf(TEXT("key '%s' parses"), *Key),
				FIssueKey::ValidateKey(Key));
		});

		It("emits zero-empty File/Line evidence fields when the diagnostic has no file", [this]()
		{
			auto Provider = MakeUnique<FFakeCompileStatusProvider>();
			Provider->State = ECompileState::Failed;
			// File empty, line/column 0 — the rule falls back to "(project)"
			// AssetPath so the FIssueKey stays well-formed.
			Provider->Diagnostics.Add(MakeDiagnostic(
				FString(), 0, 0, TEXT("linker error LNK2019"), FString()));

			FCompileErrorsRule Rule(MoveTemp(Provider));

			FVerifyScope Scope;

			TArray<FVerifyIssue> Issues;
			Rule.Scan(Scope, EVerifyRunMode::Full, Issues);

			TestEqual(TEXT("one issue"), Issues.Num(), 1);
			if (Issues.Num() != 1)
			{
				return;
			}

			const FVerifyIssue& Issue = Issues[0];
			TestEqual(TEXT("AssetPath is project sentinel"),
				Issue.AssetPath, FString(UnrealOpenMcpVerify::CompileErrors::ProjectAssetPath));
			TestEqual(TEXT("evidence file empty"), Issue.Evidence[TEXT("file")], FString());
		});
	});

	Describe("Scan - failed with no per-file diagnostics (coarse fallback)", [this]()
	{
		It("emits one coarse '(project)' finding when Failed + empty diagnostics", [this]()
		{
			auto Provider = MakeUnique<FFakeCompileStatusProvider>();
			Provider->State = ECompileState::Failed;
			// Diagnostics intentionally empty — the provider knows the project
			// failed but has no per-file breakdown (the v1 Live Coding path).

			FCompileErrorsRule Rule(MoveTemp(Provider));

			FVerifyScope Scope;

			TArray<FVerifyIssue> Issues;
			Rule.Scan(Scope, EVerifyRunMode::Full, Issues);

			TestEqual(TEXT("one coarse issue"), Issues.Num(), 1);
			if (Issues.Num() != 1)
			{
				return;
			}

			const FVerifyIssue& Issue = Issues[0];
			TestEqual(TEXT("ruleId"), Issue.RuleId, FString(UnrealOpenMcpVerify::CompileErrors::RuleId));
			TestEqual(TEXT("severity"), Issue.Severity, EVerifySeverity::Error);
			TestEqual(TEXT("AssetPath is project sentinel"),
				Issue.AssetPath, FString(UnrealOpenMcpVerify::CompileErrors::ProjectAssetPath));
			TestEqual(TEXT("source = coarse"),
				Issue.Evidence[TEXT("source")], FString(TEXT("coarse")));
			TestTrue(TEXT("issueCode starts with bare code"),
				Issue.IssueCode.StartsWith(UnrealOpenMcpVerify::CompileErrors::IssueCode));

			// The coarse finding's FIssueKey must still parse.
			const FString Key = FIssueKey::Build(Issue);
			TestTrue(
				FString::Printf(TEXT("coarse key '%s' parses"), *Key),
				FIssueKey::ValidateKey(Key));
		});
	});

	Describe("Scan - paths_hint filtering", [this]()
	{
		It("filters per-file findings by File prefix under a non-empty paths_hint", [this]()
		{
			auto Provider = MakeUnique<FFakeCompileStatusProvider>();
			Provider->State = ECompileState::Failed;
			Provider->Diagnostics.Add(MakeDiagnostic(
				TEXT("Source/ModuleA/Foo.cpp"), 10, 0,
				TEXT("err A"), TEXT("ModuleA")));
			Provider->Diagnostics.Add(MakeDiagnostic(
				TEXT("Source/ModuleB/Bar.cpp"), 20, 0,
				TEXT("err B"), TEXT("ModuleB")));

			FCompileErrorsRule Rule(MoveTemp(Provider));

			TArray<FString> Paths;
			Paths.Add(TEXT("Source/ModuleA"));
			FVerifyScope Scope(MoveTemp(Paths));

			TArray<FVerifyIssue> Issues;
			Rule.Scan(Scope, EVerifyRunMode::Validate, Issues);

			TestEqual(TEXT("only the in-scope diagnostic emitted"), Issues.Num(), 1);
			if (Issues.Num() != 1)
			{
				return;
			}
			TestEqual(TEXT("in-scope file"),
				Issues[0].AssetPath, FString(TEXT("Source/ModuleA/Foo.cpp")));
		});

		It("still emits the coarse '(project)' finding when paths_hint excludes every per-file diagnostic", [this]()
		{
			auto Provider = MakeUnique<FFakeCompileStatusProvider>();
			Provider->State = ECompileState::Failed;
			Provider->Diagnostics.Add(MakeDiagnostic(
				TEXT("Source/ModuleA/Foo.cpp"), 10, 0,
				TEXT("err A"), TEXT("ModuleA")));

			FCompileErrorsRule Rule(MoveTemp(Provider));

			TArray<FString> Paths;
			Paths.Add(TEXT("Source/Unrelated"));
			FVerifyScope Scope(MoveTemp(Paths));

			TArray<FVerifyIssue> Issues;
			Rule.Scan(Scope, EVerifyRunMode::Validate, Issues);

			// The per-file diagnostic is filtered out by paths_hint, but the
			// coarse "(project)" finding is still emitted — a known failure
			// must never be silently swallowed, even outside the touched paths.
			TestEqual(TEXT("coarse fallback emitted"), Issues.Num(), 1);
			if (Issues.Num() != 1)
			{
				return;
			}
			TestEqual(TEXT("AssetPath is project sentinel"),
				Issues[0].AssetPath, FString(UnrealOpenMcpVerify::CompileErrors::ProjectAssetPath));
		});
	});

	Describe("FCompileErrorsRule - fix-matching contract", [this]()
	{
		It("emits issues whose IssueCode bare form matches the rule catalog", [this]()
		{
			// P3.7 will register fix providers keyed by (ruleId + bare issueCode).
			// Pin the contract here so a future fix can match without surprises.
			auto Provider = MakeUnique<FFakeCompileStatusProvider>();
			Provider->State = ECompileState::Failed;
			Provider->Diagnostics.Add(MakeDiagnostic(
				TEXT("Source/MyModule/Foo.cpp"), 42, 5,
				TEXT("expected ';'"), TEXT("")));

			FCompileErrorsRule Rule(MoveTemp(Provider));

			FVerifyScope Scope;

			TArray<FVerifyIssue> Issues;
			Rule.Scan(Scope, EVerifyRunMode::Full, Issues);

			TestEqual(TEXT("one issue"), Issues.Num(), 1);
			if (Issues.Num() != 1)
			{
				return;
			}

			const FString Bare = FIssueKey::BareIssueCode(Issues[0].IssueCode);
			TestEqual(TEXT("bare issueCode"), Bare, FString(UnrealOpenMcpVerify::CompileErrors::IssueCode));
			TestEqual(TEXT("ruleId"), Issues[0].RuleId, FString(UnrealOpenMcpVerify::CompileErrors::RuleId));
		});
	});

	Describe("FCompileErrorsRule - no compile side effects", [this]()
	{
		It("only calls the read-only provider methods (GetState, GetDiagnostics)", [this]()
		{
			// The hard ban from specs/execution/P3/P3.4.md: the rule must not
			// trigger a compile. The provider interface exposes only read-only
			// methods (no Compile() to call), so this test pins the contract
			// by counting provider calls and asserting the rule never reaches
			// for any method outside the read-only surface.
			auto Provider = MakeUnique<FFakeCompileStatusProvider>();
			Provider->State = ECompileState::Failed;
			Provider->Diagnostics.Add(MakeDiagnostic(
				TEXT("Source/MyModule/Foo.cpp"), 42, 0,
				TEXT("expected ';'"), TEXT("")));

			FFakeCompileStatusProvider* ProviderPtr = Provider.Get();
			FCompileErrorsRule Rule(MoveTemp(Provider));

			FVerifyScope Scope;

			TArray<FVerifyIssue> Issues;
			Rule.Scan(Scope, EVerifyRunMode::Full, Issues);

			// Read-only surface: GetState was called once; GetDiagnostics was
			// called once (only because State == Failed). No other entry
			// point exists on ICompileStatusProvider.
			TestEqual(TEXT("GetState called once"), ProviderPtr->GetStateCallCount, 1);
			TestEqual(TEXT("GetDiagnostics called once"), ProviderPtr->GetDiagnosticsCallCount, 1);
		});

		It("does not consult GetDiagnostics on a Clean state", [this]()
		{
			// Cheaper-than-baseline path: when the state is Clean, the rule
			// must skip the (potentially expensive) diagnostics fetch.
			auto Provider = MakeUnique<FFakeCompileStatusProvider>();
			Provider->State = ECompileState::Clean;

			FFakeCompileStatusProvider* ProviderPtr = Provider.Get();
			FCompileErrorsRule Rule(MoveTemp(Provider));

			FVerifyScope Scope;

			TArray<FVerifyIssue> Issues;
			Rule.Scan(Scope, EVerifyRunMode::Full, Issues);

			TestEqual(TEXT("GetState called once"), ProviderPtr->GetStateCallCount, 1);
			TestEqual(TEXT("GetDiagnostics not called on Clean"), ProviderPtr->GetDiagnosticsCallCount, 0);
		});
	});

	Describe("FCompileErrorsRule - default-constructed (production provider)", [this]()
	{
		It("constructs without a provider and reports the stable Id", [this]()
		{
			// The default constructor wires the production Live Coding
			// provider. We do not exercise Scan() here because the production
			// provider's behavior depends on the host editor / platform
			// (PLATFORM_WINDOWS / Live Coding loaded / IsCompiling). The
			// injected-provider cases above pin the rule's logic; this case
			// pins the default-construction + GetId contract only.
			FCompileErrorsRule Rule;
			TestEqual(TEXT("id"), Rule.GetId(), FString(UnrealOpenMcpVerify::CompileErrors::RuleId));
		});

		It("emits nothing under the default provider in a Clean / unknown environment", [this]()
		{
			// The v1 production provider deliberately returns Clean on
			// platforms without Live Coding and InProgressOrUnknown on a
			// Windows editor with Live Coding active but not compiling
			// (the public read-only API cannot reliably observe the last
			// compile result). In both cases the rule must emit nothing —
			// no false positives on a clean project. See the provider's
			// implementation comment for the v1 fidelity rationale.
			FCompileErrorsRule Rule;

			TArray<FString> Paths;
			Paths.Add(TEXT("Source/MyModule/Foo.cpp"));
			FVerifyScope Scope(MoveTemp(Paths));

			TArray<FVerifyIssue> Issues;
			Rule.Scan(Scope, EVerifyRunMode::Full, Issues);

			TestEqual(TEXT("no findings from default provider"), Issues.Num(), 0);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
