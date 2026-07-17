// FVerifyResult / FVerifyScope / FCheckpointFingerprint Automation specs
// (P3.1).
//
// Ports Unity Open MCP's VerifyResultTests
// (packages/verify/Tests/Editor/VerifyResultTests.cs) to Unreal at copy
// fidelity. Pins the property-bag contract these value types carry: the
// result's Issues/CategoriesRun/DurationMs/UnknownRuleIds/AvailableRuleIds
// fields and the HasUnknownRules predicate; the scope's Paths +
// bIncludeDependents defaults; and the checkpoint fingerprint's per-rule
// Errors / Warnings / IssueKeys map.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Core/CheckpointFingerprint.h"
#include "Core/VerifyIssue.h"
#include "Core/VerifyResult.h"
#include "Core/VerifyScope.h"
#include "Core/VerifySeverity.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpVerifyResultSpec,
	"UnrealOpenMcp.Verify.Result",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpVerifyResultSpec)

void FUnrealOpenMcpVerifyResultSpec::Define()
{
	Describe("FVerifyResult", [this]()
	{
		It("sets Issues / CategoriesRun / DurationMs from the 3-arg ctor", [this]()
		{
			TArray<FVerifyIssue> Issues;
			Issues.Emplace(
				TEXT("rule_a"), EVerifySeverity::Warning,
				TEXT("/Game/A.uasset"), TEXT("code"), TEXT("desc"));

			TArray<FString> Categories;
			Categories.Add(TEXT("rule_a"));

			const FVerifyResult Result(MoveTemp(Issues), MoveTemp(Categories), 42);

			TestEqual(TEXT("issue count"), Result.Issues.Num(), 1);
			TestEqual(TEXT("category"), Result.CategoriesRun[0], FString(TEXT("rule_a")));
			TestEqual(TEXT("duration"), Result.DurationMs, static_cast<int64>(42));
		});

		It("defaults HasUnknownRules to false", [this]()
		{
			const FVerifyResult Result(TArray<FVerifyIssue>(), TArray<FString>(), 0);
			TestFalse(TEXT("HasUnknownRules"), Result.HasUnknownRules());
			TestEqual(TEXT("UnknownRuleIds empty"), Result.UnknownRuleIds.Num(), 0);
			TestEqual(TEXT("AvailableRuleIds empty"), Result.AvailableRuleIds.Num(), 0);
		});

		It("surfaces UnknownRuleIds + AvailableRuleIds from the 5-arg ctor", [this]()
		{
			TArray<FString> Unknown;
			Unknown.Add(TEXT("ghost"));
			TArray<FString> Available;
			Available.Add(TEXT("real"));

			const FVerifyResult Result(
				TArray<FVerifyIssue>(), TArray<FString>(), 0,
				MoveTemp(Unknown), MoveTemp(Available));

			TestTrue(TEXT("HasUnknownRules"), Result.HasUnknownRules());
			TestEqual(TEXT("UnknownRuleIds[0]"), Result.UnknownRuleIds[0], FString(TEXT("ghost")));
			TestEqual(TEXT("AvailableRuleIds[0]"), Result.AvailableRuleIds[0], FString(TEXT("real")));
		});
	});

	Describe("FVerifyScope", [this]()
	{
		It("defaults bIncludeDependents to false", [this]()
		{
			TArray<FString> Paths;
			Paths.Add(TEXT("/Game/A.uasset"));

			const FVerifyScope Scope(MoveTemp(Paths));

			TestEqual(TEXT("paths"), Scope.Paths[0], FString(TEXT("/Game/A.uasset")));
			TestFalse(TEXT("bIncludeDependents"), Scope.bIncludeDependents);
		});

		It("honors the explicit bIncludeDependents flag", [this]()
		{
			TArray<FString> Paths;
			Paths.Add(TEXT("/Game/A.uasset"));

			const FVerifyScope Scope(MoveTemp(Paths), /*bIncludeDependents*/ true);

			TestTrue(TEXT("bIncludeDependents"), Scope.bIncludeDependents);
		});

		It("defaults to an empty path set", [this]()
		{
			const FVerifyScope Scope;
			TestEqual(TEXT("empty paths"), Scope.Paths.Num(), 0);
			TestFalse(TEXT("bIncludeDependents default"), Scope.bIncludeDependents);
		});
	});

	Describe("FCheckpointFingerprint", [this]()
	{
		It("carries CheckpointId and the per-rule map", [this]()
		{
			TSet<FString> Keys;
			Keys.Add(TEXT("key1"));
			TMap<FString, FRuleFingerprint> Fingerprints;
			Fingerprints.Add(TEXT("rule_a"), FRuleFingerprint(/*Errors*/ 1, /*Warnings*/ 2, MoveTemp(Keys)));

			const FCheckpointFingerprint Cp(TEXT("cp_test"), MoveTemp(Fingerprints));

			TestEqual(TEXT("CheckpointId"), Cp.CheckpointId, FString(TEXT("cp_test")));
			TestEqual(TEXT("fingerprint count"), Cp.Fingerprints.Num(), 1);
			TestTrue(TEXT("has rule_a"), Cp.Fingerprints.Contains(TEXT("rule_a")));
			TestEqual(TEXT("errors"), Cp.Fingerprints[TEXT("rule_a")].Errors, 1);
			TestEqual(TEXT("warnings"), Cp.Fingerprints[TEXT("rule_a")].Warnings, 2);
			TestTrue(TEXT("carries key1"), Cp.Fingerprints[TEXT("rule_a")].IssueKeys.Contains(TEXT("key1")));
		});

		It("defaults to an empty map", [this]()
		{
			const FCheckpointFingerprint Cp;
			TestTrue(TEXT("CheckpointId empty"), Cp.CheckpointId.IsEmpty());
			TestEqual(TEXT("empty map"), Cp.Fingerprints.Num(), 0);
		});
	});

	Describe("FVerifyIssue", [this]()
	{
		It("constructs via the 5-arg ctor with empty Evidence", [this]()
		{
			const FVerifyIssue Issue(
				TEXT("rule_a"), EVerifySeverity::Error,
				TEXT("/Game/A.uasset"), TEXT("missing_script"), TEXT("desc"));

			TestEqual(TEXT("RuleId"), Issue.RuleId, FString(TEXT("rule_a")));
			TestEqual(TEXT("Severity"), Issue.Severity, EVerifySeverity::Error);
			TestEqual(TEXT("AssetPath"), Issue.AssetPath, FString(TEXT("/Game/A.uasset")));
			TestEqual(TEXT("IssueCode"), Issue.IssueCode, FString(TEXT("missing_script")));
			TestEqual(TEXT("Description"), Issue.Description, FString(TEXT("desc")));
			TestEqual(TEXT("Evidence empty"), Issue.Evidence.Num(), 0);
		});

		It("constructs via the 6-arg ctor with Evidence", [this]()
		{
			TMap<FString, FString> Evidence;
			Evidence.Add(TEXT("guid"), TEXT("abc"));

			const FVerifyIssue Issue(
				TEXT("rule_a"), EVerifySeverity::Error,
				TEXT("/Game/A.uasset"), TEXT("missing_guid"), TEXT("desc"),
				MoveTemp(Evidence));

			TestEqual(TEXT("Evidence[guid]"), Issue.Evidence[TEXT("guid")], FString(TEXT("abc")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
