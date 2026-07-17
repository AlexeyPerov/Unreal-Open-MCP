// FVerifyRunner Automation specs (P3.1).
//
// Ports Unity Open MCP's VerifyRunnerTests
// (packages/verify/Tests/Editor/VerifyRunnerTests.cs) to Unreal at copy
// fidelity, using stub rules implemented in the spec file (Unity uses nested
// NUnit stub classes; the Unreal port uses lambdas / small local classes).
// Pins:
//   - empty-scan behavior (the P3.1 scaffold has no default rules yet)
//   - requested-vs-registered split: UnknownRuleIds / AvailableRuleIds
//   - dispatch only to the requested subset when RuleIds is non-empty
//   - rule scan exceptions are swallowed so one bad rule cannot abort a pass
//     (WITH_EXCEPTIONS-guarded; in no-exceptions builds a misbehaving rule
//     surfaces as a crash)
//   - DurationMs is recorded (≥0)
//   - every emitted issue has a parseable FIssueKey
//   - CreateCheckpoint produces a cp_xxxxxx Id and per-rule fingerprints
//
// Stub rules use unreal content paths (/Game/.../X.uasset) so the issue keys
// are well-formed for FIssueKey.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Core/IVerifyRule.h"
#include "Core/IssueKey.h"
#include "Core/VerifyIssue.h"
#include "Core/VerifyRunMode.h"
#include "Core/VerifyRunner.h"
#include "Core/VerifyScope.h"
#include "Core/VerifySeverity.h"

#include <stdexcept> // std::runtime_error in the FThrowingRule stub (WITH_EXCEPTIONS only)

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpVerifyRunnerSpec,
	"UnrealOpenMcp.Verify.Runner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpVerifyRunnerSpec)

namespace
{

// Stub rule that emits exactly one Warning per scan. Used to pin dispatch
// without coupling to a real rule family.
class FStubRule : public IVerifyRule
{
public:
	FString Id;
	explicit FStubRule(FString InId) : Id(MoveTemp(InId)) {}

	virtual FString GetId() const override { return Id; }

	virtual void Scan(const FVerifyScope& Scope, const EVerifyRunMode Mode, TArray<FVerifyIssue>& Sink) const override
	{
		Sink.Emplace(Id, EVerifySeverity::Warning,
			TEXT("/Game/Fixtures/Test.uasset"), TEXT("stub_issue"), TEXT("stub description"));
	}
};

// Stub rule that emits one Error + two Warnings (different asset paths so
// the issue keys are distinct). Used to pin the per-rule fingerprint counts.
class FMultiIssueRule : public IVerifyRule
{
public:
	FString Id;
	explicit FMultiIssueRule(FString InId) : Id(MoveTemp(InId)) {}

	virtual FString GetId() const override { return Id; }

	virtual void Scan(const FVerifyScope& Scope, const EVerifyRunMode Mode, TArray<FVerifyIssue>& Sink) const override
	{
		Sink.Emplace(Id, EVerifySeverity::Error,
			TEXT("/Game/Fixtures/A.uasset"), TEXT("err_1"), TEXT("error"));
		Sink.Emplace(Id, EVerifySeverity::Warning,
			TEXT("/Game/Fixtures/A.uasset"), TEXT("warn_1"), TEXT("warning"));
		Sink.Emplace(Id, EVerifySeverity::Warning,
			TEXT("/Game/Fixtures/B.uasset"), TEXT("warn_2"), TEXT("warning"));
	}
};

// Stub rule that throws during Scan. The runner must swallow the exception
// (WITH_EXCEPTIONS only) so a bad rule cannot abort a gate pass.
class FThrowingRule : public IVerifyRule
{
public:
	FString Id;
	explicit FThrowingRule(FString InId) : Id(MoveTemp(InId)) {}

	virtual FString GetId() const override { return Id; }

	virtual void Scan(const FVerifyScope& Scope, const EVerifyRunMode Mode, TArray<FVerifyIssue>& Sink) const override
	{
		throw std::runtime_error("test exception");
	}
};

// RAII guard: clear the rule registry on construct / destruct so each spec
// starts and ends with a clean slate (Unity's SetUp / TearDown does the
// same). The registry is process-global static state; without this guard a
// later spec would see a previous spec's leftover rules.
struct FScopedRules
{
	FScopedRules() { FVerifyRunner::ClearRules(); }
	~FScopedRules() { FVerifyRunner::ClearRules(); }
};

} // namespace

void FUnrealOpenMcpVerifyRunnerSpec::Define()
{
	Describe("RunScoped — empty registry", [this]()
	{
		It("returns an empty result with no rules registered", [this]()
		{
			FScopedRules Guard;
			TArray<FString> Paths;
			Paths.Add(TEXT("/Game/Dummy.uasset"));
			const FVerifyScope Scope(MoveTemp(Paths));

			const FVerifyResult Result = FVerifyRunner::RunScoped(Scope, TArray<FString>(), EVerifyRunMode::Checkpoint);

			TestEqual(TEXT("no issues"), Result.Issues.Num(), 0);
			TestEqual(TEXT("no categories"), Result.CategoriesRun.Num(), 0);
			TestFalse(TEXT("no unknowns"), Result.HasUnknownRules());
		});
	});

	Describe("RunScoped — requested-vs-registered split", [this]()
	{
		It("reports UnknownRuleIds and AvailableRuleIds when a requested rule is missing", [this]()
		{
			FScopedRules Guard;
			FVerifyRunner::RegisterRule(MakeUnique<FStubRule>(TEXT("known_rule")));

			TArray<FString> Paths;
			Paths.Add(TEXT("/Game/Dummy.uasset"));
			const FVerifyScope Scope(MoveTemp(Paths));

			TArray<FString> Requested;
			Requested.Add(TEXT("nonexistent_rule"));

			const FVerifyResult Result = FVerifyRunner::RunScoped(Scope, Requested, EVerifyRunMode::Checkpoint);

			TestTrue(TEXT("HasUnknownRules"), Result.HasUnknownRules());
			TestEqual(TEXT("one unknown"), Result.UnknownRuleIds.Num(), 1);
			TestEqual(TEXT("unknown id"), Result.UnknownRuleIds[0], FString(TEXT("nonexistent_rule")));
			TestTrue(TEXT("available contains known_rule"),
				Result.AvailableRuleIds.Contains(TEXT("known_rule")));
		});

		It("splits mixed known + unknown requests", [this]()
		{
			FScopedRules Guard;
			FVerifyRunner::RegisterRule(MakeUnique<FStubRule>(TEXT("rule_a")));
			FVerifyRunner::RegisterRule(MakeUnique<FStubRule>(TEXT("rule_b")));

			TArray<FString> Paths;
			Paths.Add(TEXT("/Game/Dummy.uasset"));
			const FVerifyScope Scope(MoveTemp(Paths));

			TArray<FString> Requested;
			Requested.Add(TEXT("rule_a"));
			Requested.Add(TEXT("ghost_rule"));

			const FVerifyResult Result = FVerifyRunner::RunScoped(Scope, Requested, EVerifyRunMode::Checkpoint);

			TestEqual(TEXT("unknown count"), Result.UnknownRuleIds.Num(), 1);
			TestEqual(TEXT("unknown id"), Result.UnknownRuleIds[0], FString(TEXT("ghost_rule")));
			TestEqual(TEXT("categories run"), Result.CategoriesRun.Num(), 1);
			TestEqual(TEXT("category id"), Result.CategoriesRun[0], FString(TEXT("rule_a")));
		});
	});

	Describe("RunScoped — dispatch", [this]()
	{
		It("runs every registered rule when RuleIds is empty", [this]()
		{
			FScopedRules Guard;
			FVerifyRunner::RegisterRule(MakeUnique<FStubRule>(TEXT("rule_a")));
			FVerifyRunner::RegisterRule(MakeUnique<FStubRule>(TEXT("rule_b")));

			TArray<FString> Paths;
			Paths.Add(TEXT("/Game/Dummy.uasset"));
			const FVerifyScope Scope(MoveTemp(Paths));

			const FVerifyResult Result = FVerifyRunner::RunScoped(Scope, TArray<FString>(), EVerifyRunMode::Checkpoint);

			TestFalse(TEXT("no unknowns"), Result.HasUnknownRules());
			TestEqual(TEXT("ran both"), Result.CategoriesRun.Num(), 2);
		});

		It("dispatches only to the requested rule", [this]()
		{
			FScopedRules Guard;
			FVerifyRunner::RegisterRule(MakeUnique<FStubRule>(TEXT("rule_a")));
			FVerifyRunner::RegisterRule(MakeUnique<FStubRule>(TEXT("rule_b")));

			TArray<FString> Paths;
			Paths.Add(TEXT("/Game/Fixtures/Test.uasset"));
			const FVerifyScope Scope(MoveTemp(Paths));

			TArray<FString> Requested;
			Requested.Add(TEXT("rule_b"));

			const FVerifyResult Result = FVerifyRunner::RunScoped(Scope, Requested, EVerifyRunMode::Validate);

			TestEqual(TEXT("categories"), Result.CategoriesRun.Num(), 1);
			TestEqual(TEXT("category id"), Result.CategoriesRun[0], FString(TEXT("rule_b")));
			TestEqual(TEXT("one issue"), Result.Issues.Num(), 1);
			TestEqual(TEXT("issue rule"), Result.Issues[0].RuleId, FString(TEXT("rule_b")));
		});

		It("records a non-negative DurationMs", [this]()
		{
			FScopedRules Guard;
			FVerifyRunner::RegisterRule(MakeUnique<FStubRule>(TEXT("rule_a")));

			TArray<FString> Paths;
			Paths.Add(TEXT("/Game/Dummy.uasset"));
			const FVerifyScope Scope(MoveTemp(Paths));

			const FVerifyResult Result = FVerifyRunner::RunScoped(Scope, TArray<FString>(), EVerifyRunMode::Checkpoint);

			TestTrue(TEXT("DurationMs >= 0"), Result.DurationMs >= 0);
		});

		It("produces issues with parseable FIssueKeys", [this]()
		{
			FScopedRules Guard;
			FVerifyRunner::RegisterRule(MakeUnique<FStubRule>(TEXT("rule_a")));

			TArray<FString> Paths;
			Paths.Add(TEXT("/Game/Dummy.uasset"));
			const FVerifyScope Scope(MoveTemp(Paths));

			const FVerifyResult Result = FVerifyRunner::RunScoped(Scope, TArray<FString>(), EVerifyRunMode::Validate);

			for (const FVerifyIssue& Issue : Result.Issues)
			{
				const FString Key = FIssueKey::Build(Issue);
				TestTrue(
					FString::Printf(TEXT("issue key '%s' should parse"), *Key),
					FIssueKey::ValidateKey(Key));
			}
		});
	});

#if WITH_EXCEPTIONS
	Describe("RunScoped — rule exception handling (WITH_EXCEPTIONS)", [this]()
	{
		It("does not propagate an exception thrown from a rule's Scan", [this]()
		{
			FScopedRules Guard;
			FVerifyRunner::RegisterRule(MakeUnique<FThrowingRule>(TEXT("crashy")));

			TArray<FString> Paths;
			Paths.Add(TEXT("/Game/Fixtures/Test.uasset"));
			const FVerifyScope Scope(MoveTemp(Paths));

			TArray<FString> Requested;
			Requested.Add(TEXT("crashy"));

			// No ASSERT-style helper here — the expectation is simply "does
			// not crash". If RunScoped throws, the automation runner catches
			// it as a crash and the case fails.
			FVerifyRunner::RunScoped(Scope, Requested, EVerifyRunMode::Checkpoint);
			TestTrue(TEXT("reached here without throwing"), true);
		});

		It("still runs the other rules when one throws", [this]()
		{
			FScopedRules Guard;
			FVerifyRunner::RegisterRule(MakeUnique<FThrowingRule>(TEXT("crashy")));
			FVerifyRunner::RegisterRule(MakeUnique<FStubRule>(TEXT("stable")));

			TArray<FString> Paths;
			Paths.Add(TEXT("/Game/Fixtures/Test.uasset"));
			const FVerifyScope Scope(MoveTemp(Paths));

			const FVerifyResult Result = FVerifyRunner::RunScoped(Scope, TArray<FString>(), EVerifyRunMode::Checkpoint);

			// Both rules are listed as run even though crashy threw; the
			// stable rule's issue is in the sink.
			TestEqual(TEXT("categories run"), Result.CategoriesRun.Num(), 2);
			TestEqual(TEXT("one issue"), Result.Issues.Num(), 1);
			TestEqual(TEXT("issue rule"), Result.Issues[0].RuleId, FString(TEXT("stable")));
		});
	});
#endif // WITH_EXCEPTIONS

	Describe("CreateCheckpoint", [this]()
	{
		It("produces a cp_ prefix id with per-rule fingerprints", [this]()
		{
			FScopedRules Guard;
			FVerifyRunner::RegisterRule(MakeUnique<FStubRule>(TEXT("rule_a")));
			FVerifyRunner::RegisterRule(MakeUnique<FStubRule>(TEXT("rule_b")));

			TArray<FString> Paths;
			Paths.Add(TEXT("/Game/Dummy.uasset"));
			const FVerifyScope Scope(MoveTemp(Paths));

			const FCheckpointFingerprint Cp = FVerifyRunner::CreateCheckpoint(Scope, TArray<FString>());

			TestFalse(TEXT("id non-empty"), Cp.CheckpointId.IsEmpty());
			TestTrue(TEXT("id starts with cp_"), Cp.CheckpointId.StartsWith(TEXT("cp_")));
			TestEqual(TEXT("two fingerprints"), Cp.Fingerprints.Num(), 2);
			TestTrue(TEXT("has rule_a"), Cp.Fingerprints.Contains(TEXT("rule_a")));
			TestTrue(TEXT("has rule_b"), Cp.Fingerprints.Contains(TEXT("rule_b")));
		});

		It("counts Errors and Warnings per rule", [this]()
		{
			FScopedRules Guard;
			FVerifyRunner::RegisterRule(MakeUnique<FMultiIssueRule>(TEXT("rule_a")));

			TArray<FString> Paths;
			Paths.Add(TEXT("/Game/Dummy.uasset"));
			const FVerifyScope Scope(MoveTemp(Paths));

			const FCheckpointFingerprint Cp = FVerifyRunner::CreateCheckpoint(Scope, TArray<FString>());

			const FRuleFingerprint& Fp = Cp.Fingerprints[TEXT("rule_a")];
			TestEqual(TEXT("errors"), Fp.Errors, 1);
			TestEqual(TEXT("warnings"), Fp.Warnings, 2);
			TestEqual(TEXT("issue keys"), Fp.IssueKeys.Num(), 3);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
