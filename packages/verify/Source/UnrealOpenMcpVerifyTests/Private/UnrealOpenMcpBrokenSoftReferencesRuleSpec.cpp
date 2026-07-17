// FBrokenSoftReferencesRule Automation specs (P3.2).
//
// Adapts Unity Open MCP's FixtureMissingReferencesTests shape
// (packages/verify/Tests/Editor/FixtureMissingReferencesTests.cs) to Unreal.
// Unity authors .prefab fixtures on disk; the Unreal port instead spawns a
// transient UPackage + a test-only UObject with TSoftObjectPtr<> properties,
// then drives the scanner via ScanPackage with a fake ISoftPathResolver. The
// fake decouples the spec from the Asset Registry so cases are deterministic
// without authoring .uasset fixtures.
//
// Pins (acceptance criteria from specs/execution/P3/P3.2.md):
//   - Broken soft refs surface as broken_soft_reference Errors
//   - Valid soft refs do not emit issues (false-positive guard)
//   - Empty / unset soft pointers never emit (false-positive guard)
//   - The rule is registered by RegisterDefaults with the stable Id
//   - Every emitted issue has a parseable FIssueKey and the right evidence
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
#include "Fixtures/BrokenSoftRefFixtureObject.h"
#include "Rules/BrokenSoftReferences/BrokenSoftReferencesIssueCodes.h"
#include "Rules/BrokenSoftReferences/BrokenSoftReferencesRule.h"
#include "Rules/BrokenSoftReferences/ISoftPathResolver.h"

// Scanner private header lives under the verify module's Private/ tree; the
// tests Build.cs adds that folder to its PrivateIncludePaths.
#include "Rules/BrokenSoftReferences/BrokenSoftReferencesScanner.h"

#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpBrokenSoftReferencesRuleSpec,
	"UnrealOpenMcp.Verify.BrokenSoftReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpBrokenSoftReferencesRuleSpec)

namespace
{

// Fake resolver: a path resolves iff it is in the allow-set. Cheap and
// deterministic -- no Asset Registry / LoadPackage touched.
class FFakeSoftPathResolver final : public ISoftPathResolver
{
public:
	TSet<FString> Resolvable;

	virtual bool Resolve(const FString& SoftPath) const override
	{
		return Resolvable.Contains(SoftPath);
	}
};

// RAII guard mirroring FScopedRules in the runner spec: clear the rule
// registry on construct / destruct so each spec starts and ends clean.
struct FScopedRules
{
	FScopedRules() { FVerifyRunner::ClearRules(); }
	~FScopedRules() { FVerifyRunner::ClearRules(); }
};

// Build a fresh transient package + fixture object for one test. Returns
// nullptr for either out-param on failure so the caller can skip the case
// rather than crash the automation runner.
struct FFixture
{
	UPackage* Package = nullptr;
	UBrokenSoftRefFixtureObject* Object = nullptr;
};

FFixture MakeFixture(const FString& PackageName)
{
	FFixture F;
	F.Package = NewObject<UPackage>(nullptr, FName(*PackageName), RF_Transient);
	if (F.Package == nullptr)
	{
		return F;
	}
	// Outer the fixture object under the package so GetObjectsWithOuter
	// finds it.
	F.Object = NewObject<UBrokenSoftRefFixtureObject>(F.Package);
	return F;
}

} // namespace

void FUnrealOpenMcpBrokenSoftReferencesRuleSpec::Define()
{
	Describe("RegisterDefaults", [this]()
	{
		It("registers broken_soft_references with the stable Id", [this]()
		{
			FScopedRules Guard;
			FVerifyRunner::RegisterDefaults();

			const auto& Rules = FVerifyRunner::GetRules();
			bool bFound = false;
			for (const TUniquePtr<IVerifyRule>& Rule : Rules)
			{
				if (Rule.IsValid() && Rule->GetId() == UnrealOpenMcpVerify::BrokenSoftReferences::RuleId)
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
				if (Rule.IsValid() && Rule->GetId() == UnrealOpenMcpVerify::BrokenSoftReferences::RuleId)
				{
					++Count;
				}
			}
			TestEqual(TEXT("single registration"), Count, 1);
		});
	});

	Describe("FBrokenSoftReferencesRule::GetId", [this]()
	{
		It("returns the stable rule Id", [this]()
		{
			FBrokenSoftReferencesRule Rule;
			TestEqual(TEXT("id"), Rule.GetId(), FString(UnrealOpenMcpVerify::BrokenSoftReferences::RuleId));
		});
	});

	Describe("ScanPackage - broken soft reference", [this]()
	{
		It("emits one broken_soft_reference Error for an unresolved target", [this]()
		{
			FFixture F = MakeFixture(TEXT("/Game/UnrealOpenMcpVerifyTests/BrokenFixture"));
			if (!TestNotNull(TEXT("package"), F.Package) || !TestNotNull(TEXT("object"), F.Object))
			{
				return;
			}

			// Wire the object: ValidPtr resolves, BrokenPtr does not, EmptyPtr
			// is left unset.
			const FString ValidTarget = TEXT("/Game/Fixtures/ValidTarget.ValidTarget");
			const FString BrokenTarget = TEXT("/Game/Missing/BrokenTarget.BrokenTarget");
			F.Object->ValidPtr = TSoftObjectPtr<UObject>(FSoftObjectPath(ValidTarget));
			F.Object->BrokenPtr = TSoftObjectPtr<UObject>(FSoftObjectPath(BrokenTarget));

			auto Resolver = MakeUnique<FFakeSoftPathResolver>();
			Resolver->Resolvable.Add(ValidTarget);

			TArray<FVerifyIssue> Issues;
			UnrealOpenMcpVerify::BrokenSoftReferences::ScanPackage(
				F.Package, TEXT("/Game/UnrealOpenMcpVerifyTests/BrokenFixture.BrokenFixture"),
				/*bFullScan=*/true, *Resolver, Issues);

			TestEqual(TEXT("one issue"), Issues.Num(), 1);
			if (Issues.Num() != 1)
			{
				return;
			}

			const FVerifyIssue& Issue = Issues[0];
			TestEqual(TEXT("ruleId"), Issue.RuleId, FString(UnrealOpenMcpVerify::BrokenSoftReferences::RuleId));
			TestEqual(TEXT("severity"), Issue.Severity, EVerifySeverity::Error);
			TestEqual(TEXT("issueCode starts with bare code"),
				Issue.IssueCode.StartsWith(UnrealOpenMcpVerify::BrokenSoftReferences::IssueCode), true);
			TestEqual(TEXT("assetPath"), Issue.AssetPath,
				FString(TEXT("/Game/UnrealOpenMcpVerifyTests/BrokenFixture.BrokenFixture")));
			TestTrue(TEXT("evidence has softPath"), Issue.Evidence.Contains(TEXT("softPath")));
			TestEqual(TEXT("softPath evidence"), Issue.Evidence[TEXT("softPath")], BrokenTarget);
			TestTrue(TEXT("evidence has propertyName"), Issue.Evidence.Contains(TEXT("propertyName")));
			TestEqual(TEXT("propertyName evidence"), Issue.Evidence[TEXT("propertyName")], FString(TEXT("BrokenPtr")));
		});

		It("produces a parseable FIssueKey for every finding", [this]()
		{
			FFixture F = MakeFixture(TEXT("/Game/UnrealOpenMcpVerifyTests/KeyFixture"));
			if (!TestNotNull(TEXT("object"), F.Object))
			{
				return;
			}

			F.Object->BrokenPtr = TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/Missing/Target.Target")));

			auto Resolver = MakeUnique<FFakeSoftPathResolver>();

			TArray<FVerifyIssue> Issues;
			UnrealOpenMcpVerify::BrokenSoftReferences::ScanPackage(
				F.Package, TEXT("/Game/UnrealOpenMcpVerifyTests/KeyFixture.KeyFixture"),
				/*bFullScan=*/true, *Resolver, Issues);

			for (const FVerifyIssue& Issue : Issues)
			{
				const FString Key = FIssueKey::Build(Issue);
				TestTrue(
					FString::Printf(TEXT("key '%s' parses"), *Key),
					FIssueKey::ValidateKey(Key));
			}
		});
	});

	Describe("ScanPackage - valid soft references (false-positive guard)", [this]()
	{
		It("emits no issues when every pointer resolves", [this]()
		{
			FFixture F = MakeFixture(TEXT("/Game/UnrealOpenMcpVerifyTests/ValidFixture"));
			if (!TestNotNull(TEXT("object"), F.Object))
			{
				return;
			}

			const FString TargetA = TEXT("/Game/Fixtures/A.A");
			const FString TargetB = TEXT("/Game/Fixtures/B.B");
			F.Object->ValidPtr = TSoftObjectPtr<UObject>(FSoftObjectPath(TargetA));
			F.Object->BrokenPtr = TSoftObjectPtr<UObject>(FSoftObjectPath(TargetB));

			auto Resolver = MakeUnique<FFakeSoftPathResolver>();
			Resolver->Resolvable.Add(TargetA);
			Resolver->Resolvable.Add(TargetB);

			TArray<FVerifyIssue> Issues;
			UnrealOpenMcpVerify::BrokenSoftReferences::ScanPackage(
				F.Package, TEXT("/Game/UnrealOpenMcpVerifyTests/ValidFixture.ValidFixture"),
				/*bFullScan=*/true, *Resolver, Issues);

			TestEqual(TEXT("no issues"), Issues.Num(), 0);
		});

		It("never emits for an unset soft pointer", [this]()
		{
			FFixture F = MakeFixture(TEXT("/Game/UnrealOpenMcpVerifyTests/EmptyFixture"));
			if (!TestNotNull(TEXT("object"), F.Object))
			{
				return;
			}

			// Leave every pointer unset. Even with a resolver that resolves
			// nothing, the unset pointers must not surface as findings.
			auto Resolver = MakeUnique<FFakeSoftPathResolver>();

			TArray<FVerifyIssue> Issues;
			UnrealOpenMcpVerify::BrokenSoftReferences::ScanPackage(
				F.Package, TEXT("/Game/UnrealOpenMcpVerifyTests/EmptyFixture.EmptyFixture"),
				/*bFullScan=*/true, *Resolver, Issues);

			TestEqual(TEXT("no issues for unset pointers"), Issues.Num(), 0);
		});
	});

	Describe("FBrokenSoftReferencesRule::Scan - empty scope", [this]()
	{
		It("emits nothing and does not throw on an empty paths_hint", [this]()
		{
			FBrokenSoftReferencesRule Rule;
			TArray<FVerifyIssue> Issues;
			FVerifyScope Scope; // empty Paths

			Rule.Scan(Scope, EVerifyRunMode::Full, Issues);

			TestEqual(TEXT("no issues"), Issues.Num(), 0);
		});
	});

	Describe("FBrokenSoftReferencesRule - resolver injection", [this]()
	{
		It("uses the injected resolver when constructed with one", [this]()
		{
			// Construct the rule with a fake resolver that resolves nothing.
			// The rule will not load any packages because the scope path does
			// not exist on disk -- so the resolver is never consulted. The
			// case still pins that the injection constructor compiles and
			// produces a usable rule (the GetId contract).
			auto Resolver = MakeUnique<FFakeSoftPathResolver>();
			FBrokenSoftReferencesRule Rule(MoveTemp(Resolver));

			TestEqual(TEXT("id"), Rule.GetId(), FString(UnrealOpenMcpVerify::BrokenSoftReferences::RuleId));

			TArray<FString> Paths;
			Paths.Add(TEXT("/Game/Does/Not/Exist"));
			FVerifyScope Scope(MoveTemp(Paths));

			TArray<FVerifyIssue> Issues;
			Rule.Scan(Scope, EVerifyRunMode::Full, Issues);

			// The referencing package does not load -> the scanner skips it
			// silently. No findings.
			TestEqual(TEXT("no issues for missing package"), Issues.Num(), 0);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
