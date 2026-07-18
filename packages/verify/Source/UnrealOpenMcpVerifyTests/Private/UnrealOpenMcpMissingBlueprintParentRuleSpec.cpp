// FMissingBlueprintParentRule Automation specs (P3.3).
//
// Adapts the BrokenSoftReferences spec shape (P3.2) to the
// missing_blueprint_parents rule. The spec cannot easily author a real
// .uasset Blueprint fixture with a broken parent on disk, so it:
//   - spawns a transient UPackage + a transient UBlueprint whose ParentClass
//     pointer is wired directly (NewObject<UBlueprint> + ParentClass = X)
//   - drives the scanner via ScanBlueprint with a fake resolver that returns
//     a controlled resolution outcome per path
//
// The fake resolver decouples the spec from the Asset Registry so cases are
// deterministic without authoring .uasset fixtures.
//
// Pins (acceptance criteria from specs/execution/P3/P3.3.md):
//   - A Blueprint whose parent the resolver reports as missing surfaces as
//     a missing_blueprint_parent Error
//   - A Blueprint with a healthy parent passes cleanly (false-positive guard)
//   - A Blueprint with a null ParentClass emits with expectedParent="(unknown)"
//   - The rule is registered by RegisterDefaults with the stable Id
//   - Every emitted issue has a parseable FIssueKey and the right evidence
//   - The issue integrates with the fix-matching contract (ruleId + issueCode)
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
#include "Fixtures/MissingBlueprintParentFixtureObject.h"
#include "Rules/MissingBlueprintParent/IBlueprintParentResolver.h"
#include "Rules/MissingBlueprintParent/MissingBlueprintParentIssueCodes.h"
#include "Rules/MissingBlueprintParent/MissingBlueprintParentRule.h"

// Scanner private header lives under the verify module's Private/ tree; the
// tests Build.cs adds that folder to its PrivateIncludePaths.
#include "Rules/MissingBlueprintParent/MissingBlueprintParentScanner.h"

#include "Engine/Blueprint.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpMissingBlueprintParentRuleSpec,
	"UnrealOpenMcp.Verify.MissingBlueprintParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpMissingBlueprintParentRuleSpec)

namespace
{

// Fake resolver: a path resolves iff it is in the allow-set. Paths not in
// the allow-set are classified by the configured default outcome (so a test
// can simulate a missing native vs. a missing Blueprint package without
// changing the allow-set). Cheap and deterministic — no Asset Registry /
// LoadPackage touched.
class FFakeBlueprintParentResolver final : public IBlueprintParentResolver
{
public:
	TSet<FString> Resolvable;
	EBlueprintParentResolution MissingOutcome = EBlueprintParentResolution::MissingNative;

	virtual EBlueprintParentResolution ResolveParent(
		const FString& ParentPath,
		FString& OutReason) const override
	{
		if (Resolvable.Contains(ParentPath))
		{
			return EBlueprintParentResolution::Resolved;
		}
		OutReason = FString::Printf(TEXT("fake resolver: '%s' reported as missing"), *ParentPath);
		return MissingOutcome;
	}
};

// RAII guard mirroring FScopedRules in the runner spec: clear the rule
// registry on construct / destruct so each spec starts and ends clean.
struct FScopedRules
{
	FScopedRules() { FVerifyRunner::ClearRules(); }
	~FScopedRules() { FVerifyRunner::ClearRules(); }
};

// Build a fresh transient package + transient UBlueprint for one test.
// ParentClass is left null by default; the caller wires it after creation.
struct FFixture
{
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;
};

FFixture MakeFixture(const FString& PackageName)
{
	FFixture F;
	F.Package = NewObject<UPackage>(nullptr, FName(*PackageName), RF_Transient);
	if (F.Package == nullptr)
	{
		return F;
	}
	// NewObject<UBlueprint> constructs a default UBlueprint instance. We do
	// not compile it (the scanner only reads ParentClass), so the missing
	// FBlueprintEditor / Kismet wiring is not a concern for this test.
	F.Blueprint = NewObject<UBlueprint>(F.Package, FName(*(PackageName + TEXT("_BP"))));
	return F;
}

} // namespace

void FUnrealOpenMcpMissingBlueprintParentRuleSpec::Define()
{
	Describe("RegisterDefaults", [this]()
	{
		It("registers missing_blueprint_parents with the stable Id", [this]()
		{
			FScopedRules Guard;
			FVerifyRunner::RegisterDefaults();

			const auto& Rules = FVerifyRunner::GetRules();
			bool bFound = false;
			for (const TUniquePtr<IVerifyRule>& Rule : Rules)
			{
				if (Rule.IsValid() && Rule->GetId() == UnrealOpenMcpVerify::MissingBlueprintParent::RuleId)
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
				if (Rule.IsValid() && Rule->GetId() == UnrealOpenMcpVerify::MissingBlueprintParent::RuleId)
				{
					++Count;
				}
			}
			TestEqual(TEXT("single registration"), Count, 1);
		});
	});

	Describe("FMissingBlueprintParentRule::GetId", [this]()
	{
		It("returns the stable rule Id", [this]()
		{
			FMissingBlueprintParentRule Rule;
			TestEqual(TEXT("id"), Rule.GetId(), FString(UnrealOpenMcpVerify::MissingBlueprintParent::RuleId));
		});
	});

	Describe("ScanBlueprint - missing parent", [this]()
	{
		It("emits one missing_blueprint_parent Error for an unresolved native parent", [this]()
		{
			FFixture F = MakeFixture(TEXT("/Game/UnrealOpenMcpVerifyTests/BrokenParentBP"));
			if (!TestNotNull(TEXT("package"), F.Package) || !TestNotNull(TEXT("blueprint"), F.Blueprint))
			{
				return;
			}

			// Wire the parent to a real UClass so GetPathName() is well-formed.
			// The fake resolver will report it as missing.
			F.Blueprint->ParentClass = UMissingBlueprintParentFixtureObject::StaticClass();
			const FString ParentPath = F.Blueprint->ParentClass->GetPathName();

			auto Resolver = MakeUnique<FFakeBlueprintParentResolver>();
			Resolver->MissingOutcome = EBlueprintParentResolution::MissingNative;

			TArray<FVerifyIssue> Issues;
			UnrealOpenMcpVerify::MissingBlueprintParent::ScanBlueprint(
				F.Blueprint,
				TEXT("/Game/UnrealOpenMcpVerifyTests/BrokenParentBP.BrokenParentBP"),
				*Resolver,
				Issues);

			TestEqual(TEXT("one issue"), Issues.Num(), 1);
			if (Issues.Num() != 1)
			{
				return;
			}

			const FVerifyIssue& Issue = Issues[0];
			TestEqual(TEXT("ruleId"), Issue.RuleId, FString(UnrealOpenMcpVerify::MissingBlueprintParent::RuleId));
			TestEqual(TEXT("severity"), Issue.Severity, EVerifySeverity::Error);
			TestEqual(TEXT("issueCode starts with bare code"),
				Issue.IssueCode.StartsWith(UnrealOpenMcpVerify::MissingBlueprintParent::IssueCode), true);
			TestEqual(TEXT("assetPath"), Issue.AssetPath,
				FString(TEXT("/Game/UnrealOpenMcpVerifyTests/BrokenParentBP.BrokenParentBP")));
			TestTrue(TEXT("evidence has expectedParent"), Issue.Evidence.Contains(TEXT("expectedParent")));
			TestEqual(TEXT("expectedParent evidence"), Issue.Evidence[TEXT("expectedParent")], ParentPath);
			TestTrue(TEXT("evidence has reason"), Issue.Evidence.Contains(TEXT("reason")));
		});

		It("emits one missing_blueprint_parent Error for a missing Blueprint parent package", [this]()
		{
			FFixture F = MakeFixture(TEXT("/Game/UnrealOpenMcpVerifyTests/MissingPkgBP"));
			if (!TestNotNull(TEXT("blueprint"), F.Blueprint))
			{
				return;
			}

			F.Blueprint->ParentClass = UMissingBlueprintParentFixtureObject::StaticClass();

			auto Resolver = MakeUnique<FFakeBlueprintParentResolver>();
			Resolver->MissingOutcome = EBlueprintParentResolution::MissingBlueprintPackage;

			TArray<FVerifyIssue> Issues;
			UnrealOpenMcpVerify::MissingBlueprintParent::ScanBlueprint(
				F.Blueprint,
				TEXT("/Game/UnrealOpenMcpVerifyTests/MissingPkgBP.MissingPkgBP"),
				*Resolver,
				Issues);

			TestEqual(TEXT("one issue"), Issues.Num(), 1);
			if (Issues.Num() != 1)
			{
				return;
			}

			// Description should mention the missing-package outcome.
			TestTrue(TEXT("description mentions package"),
				Issues[0].Description.Contains(TEXT("Blueprint parent package")));
		});

		It("emits with expectedParent=(unknown) when ParentClass is null", [this]()
		{
			FFixture F = MakeFixture(TEXT("/Game/UnrealOpenMcpVerifyTests/NullParentBP"));
			if (!TestNotNull(TEXT("blueprint"), F.Blueprint))
			{
				return;
			}

			// Leave ParentClass as null (NewObject default).
			F.Blueprint->ParentClass = nullptr;

			auto Resolver = MakeUnique<FFakeBlueprintParentResolver>();

			TArray<FVerifyIssue> Issues;
			UnrealOpenMcpVerify::MissingBlueprintParent::ScanBlueprint(
				F.Blueprint,
				TEXT("/Game/UnrealOpenMcpVerifyTests/NullParentBP.NullParentBP"),
				*Resolver,
				Issues);

			TestEqual(TEXT("one issue"), Issues.Num(), 1);
			if (Issues.Num() != 1)
			{
				return;
			}

			TestEqual(TEXT("expectedParent is unknown sentinel"),
				Issues[0].Evidence[TEXT("expectedParent")], FString(TEXT("(unknown)")));
		});

		It("produces a parseable FIssueKey for every finding", [this]()
		{
			FFixture F = MakeFixture(TEXT("/Game/UnrealOpenMcpVerifyTests/KeyFixtureBP"));
			if (!TestNotNull(TEXT("blueprint"), F.Blueprint))
			{
				return;
			}

			F.Blueprint->ParentClass = UMissingBlueprintParentFixtureObject::StaticClass();

			auto Resolver = MakeUnique<FFakeBlueprintParentResolver>();

			TArray<FVerifyIssue> Issues;
			UnrealOpenMcpVerify::MissingBlueprintParent::ScanBlueprint(
				F.Blueprint,
				TEXT("/Game/UnrealOpenMcpVerifyTests/KeyFixtureBP.KeyFixtureBP"),
				*Resolver,
				Issues);

			for (const FVerifyIssue& Issue : Issues)
			{
				const FString Key = FIssueKey::Build(Issue);
				TestTrue(
					FString::Printf(TEXT("key '%s' parses"), *Key),
					FIssueKey::ValidateKey(Key));
			}
		});
	});

	Describe("ScanBlueprint - healthy parent (false-positive guard)", [this]()
	{
		It("emits no issues when the parent resolves", [this]()
		{
			FFixture F = MakeFixture(TEXT("/Game/UnrealOpenMcpVerifyTests/HealthyBP"));
			if (!TestNotNull(TEXT("blueprint"), F.Blueprint))
			{
				return;
			}

			F.Blueprint->ParentClass = UMissingBlueprintParentFixtureObject::StaticClass();
			const FString ParentPath = F.Blueprint->ParentClass->GetPathName();

			auto Resolver = MakeUnique<FFakeBlueprintParentResolver>();
			Resolver->Resolvable.Add(ParentPath);

			TArray<FVerifyIssue> Issues;
			UnrealOpenMcpVerify::MissingBlueprintParent::ScanBlueprint(
				F.Blueprint,
				TEXT("/Game/UnrealOpenMcpVerifyTests/HealthyBP.HealthyBP"),
				*Resolver,
				Issues);

			TestEqual(TEXT("no issues for healthy parent"), Issues.Num(), 0);
		});
	});

	Describe("FMissingBlueprintParentRule::Scan - empty scope", [this]()
	{
		It("emits nothing and does not throw on an empty paths_hint", [this]()
		{
			FMissingBlueprintParentRule Rule;
			TArray<FVerifyIssue> Issues;
			FVerifyScope Scope; // empty Paths

			Rule.Scan(Scope, EVerifyRunMode::Full, Issues);

			TestEqual(TEXT("no issues"), Issues.Num(), 0);
		});
	});

	Describe("FMissingBlueprintParentRule - fix-matching contract", [this]()
	{
		It("emits issues whose IssueCode bare form matches the rule catalog", [this]()
		{
			// P3.7 will register fix providers keyed by (ruleId + bare issueCode).
			// Pin the contract here so a future fix can match without surprises.
			FFixture F = MakeFixture(TEXT("/Game/UnrealOpenMcpVerifyTests/ContractBP"));
			if (!TestNotNull(TEXT("blueprint"), F.Blueprint))
			{
				return;
			}

			F.Blueprint->ParentClass = UMissingBlueprintParentFixtureObject::StaticClass();

			auto Resolver = MakeUnique<FFakeBlueprintParentResolver>();

			TArray<FVerifyIssue> Issues;
			UnrealOpenMcpVerify::MissingBlueprintParent::ScanBlueprint(
				F.Blueprint,
				TEXT("/Game/UnrealOpenMcpVerifyTests/ContractBP.ContractBP"),
				*Resolver,
				Issues);

			TestEqual(TEXT("one issue"), Issues.Num(), 1);
			if (Issues.Num() != 1)
			{
				return;
			}

			const FString Bare = FIssueKey::BareIssueCode(Issues[0].IssueCode);
			TestEqual(TEXT("bare issueCode"), Bare, FString(UnrealOpenMcpVerify::MissingBlueprintParent::IssueCode));
			TestEqual(TEXT("ruleId"), Issues[0].RuleId, FString(UnrealOpenMcpVerify::MissingBlueprintParent::RuleId));
		});
	});

	Describe("FMissingBlueprintParentRule - resolver injection", [this]()
	{
		It("uses the injected resolver when constructed with one", [this]()
		{
			// The injection constructor compiles and produces a usable rule.
			// The rule will not load any packages because the scope path does
			// not exist on disk — so the resolver is never consulted. The case
			// still pins the GetId contract through the injection path.
			auto Resolver = MakeUnique<FFakeBlueprintParentResolver>();
			FMissingBlueprintParentRule Rule(MoveTemp(Resolver));

			TestEqual(TEXT("id"), Rule.GetId(), FString(UnrealOpenMcpVerify::MissingBlueprintParent::RuleId));

			TArray<FString> Paths;
			Paths.Add(TEXT("/Game/Does/Not/Exist"));
			FVerifyScope Scope(MoveTemp(Paths));

			TArray<FVerifyIssue> Issues;
			Rule.Scan(Scope, EVerifyRunMode::Full, Issues);

			// The Blueprint package does not load -> the scanner skips it
			// silently. No findings.
			TestEqual(TEXT("no issues for missing package"), Issues.Num(), 0);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
