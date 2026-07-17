// FFixProviderRegistry Automation specs (P3.1).
//
// Ports Unity Open MCP's registry-wiring tests
// (packages/verify/Tests/Editor/RelinkBrokenGuidFixTests.cs — the Registry_*
// cases + the synthetic-key strategy from FixProviderRegistry.cs) to Unreal
// at copy fidelity, using stub fix providers implemented in the spec file.
//
// Critical regression guard: TryGetFixInfo must surface the provider's REAL
// Safe flag (Unity's previous implementation hardwired safe=true and masked
// unsafe fixes as auto-applyable). The stubs cover both branches:
//   - FSafeStubProvider (bSafe=true) advertises a safe fix.
//   - FUnsafeStubProvider (bSafe=false) advertises an unsafe fix.
//   - FThrowingDescribeProvider throws from Describe — TryGetFixInfo /
//     CandidatesForIssue must default the Safe flag to false so the gate
//     never auto-applies something it cannot reason about (WITH_EXCEPTIONS).
//
// Stubs key off ruleId + issueCode via the synthetic-key strategy: the
// registry passes a placeholder asset path and each provider's CanFix
// inspects only the ruleId / issueCode fields. The same strategy is what
// the real gate uses to advertise fixes alongside a fresh scan.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Core/IssueKey.h"
#include "Fixes/FixContracts.h"
#include "Fixes/FixProviderRegistry.h"

#include <stdexcept> // std::runtime_error in the FThrowingDescribeProvider stub (WITH_EXCEPTIONS only)

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpFixProviderRegistrySpec,
	"UnrealOpenMcp.Verify.FixProviderRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpFixProviderRegistrySpec)

namespace
{

// Parse a canonical issue key into ruleId + issueCode. Returns false on any
// malformed input (the synthetic keys the registry builds always parse).
bool SplitIssueId(const FString& IssueId, FString& OutRuleId, FString& OutIssueCode)
{
	FString AssetPath;
	EVerifySeverity Severity = EVerifySeverity::Warning;
	return FIssueKey::TryParse(IssueId, OutRuleId, Severity, AssetPath, OutIssueCode);
}

// Safe stub provider: matches one ruleId + issueCode, advertises Safe=true.
class FSafeStubProvider : public IFixProvider
{
public:
	FString FixIdValue;
	FString RuleIdValue;
	FString IssueCodeValue;

	FSafeStubProvider(FString InFixId, FString InRuleId, FString InIssueCode)
		: FixIdValue(MoveTemp(InFixId))
		, RuleIdValue(MoveTemp(InRuleId))
		, IssueCodeValue(MoveTemp(InIssueCode))
	{
	}

	virtual FString GetFixId() const override { return FixIdValue; }

	virtual bool CanFix(const FString& IssueId) const override
	{
		FString RuleId, IssueCode;
		if (!SplitIssueId(IssueId, RuleId, IssueCode))
		{
			return false;
		}
		return RuleId == RuleIdValue && IssueCode == IssueCodeValue;
	}

	virtual FFixDescription Describe(const FString& IssueId) const override
	{
		FFixDescription D;
		D.FixId = FixIdValue;
		D.IssueId = IssueId;
		D.Description = TEXT("safe stub");
		D.bSafe = true;
		return D;
	}

	virtual FFixResult Apply(const FString& IssueId) override
	{
		FFixResult R;
		R.bSuccess = true;
		R.Description = TEXT("safe stub applied");
		return R;
	}
};

// Unsafe stub provider: matches one ruleId + issueCode, advertises Safe=false.
class FUnsafeStubProvider : public IFixProvider
{
public:
	FString FixIdValue;
	FString RuleIdValue;
	FString IssueCodeValue;

	FUnsafeStubProvider(FString InFixId, FString InRuleId, FString InIssueCode)
		: FixIdValue(MoveTemp(InFixId))
		, RuleIdValue(MoveTemp(InRuleId))
		, IssueCodeValue(MoveTemp(InIssueCode))
	{
	}

	virtual FString GetFixId() const override { return FixIdValue; }

	virtual bool CanFix(const FString& IssueId) const override
	{
		FString RuleId, IssueCode;
		if (!SplitIssueId(IssueId, RuleId, IssueCode))
		{
			return false;
		}
		return RuleId == RuleIdValue && IssueCode == IssueCodeValue;
	}

	virtual FFixDescription Describe(const FString& IssueId) const override
	{
		FFixDescription D;
		D.FixId = FixIdValue;
		D.IssueId = IssueId;
		D.Description = TEXT("unsafe stub");
		D.bSafe = false;
		return D;
	}

	virtual FFixResult Apply(const FString& IssueId) override
	{
		FFixResult R;
		R.bSuccess = true;
		R.Description = TEXT("unsafe stub applied");
		return R;
	}
};

#if WITH_EXCEPTIONS
// Throwing stub: matches one ruleId + issueCode but throws from Describe so
// TryGetFixInfo / CandidatesForIssue must default Safe to false.
class FThrowingDescribeProvider : public IFixProvider
{
public:
	FString FixIdValue;
	FString RuleIdValue;
	FString IssueCodeValue;

	FThrowingDescribeProvider(FString InFixId, FString InRuleId, FString InIssueCode)
		: FixIdValue(MoveTemp(InFixId))
		, RuleIdValue(MoveTemp(InRuleId))
		, IssueCodeValue(MoveTemp(InIssueCode))
	{
	}

	virtual FString GetFixId() const override { return FixIdValue; }

	virtual bool CanFix(const FString& IssueId) const override
	{
		FString RuleId, IssueCode;
		if (!SplitIssueId(IssueId, RuleId, IssueCode))
		{
			return false;
		}
		return RuleId == RuleIdValue && IssueCode == IssueCodeValue;
	}

	virtual FFixDescription Describe(const FString& IssueId) const override
	{
		throw std::runtime_error("describe failed");
	}

	virtual FFixResult Apply(const FString& IssueId) override
	{
		FFixResult R;
		R.bSuccess = false;
		R.Description = TEXT("never reached");
		return R;
	}
};
#endif // WITH_EXCEPTIONS

// RAII guard: clear the provider registry on construct / destruct so each
// spec starts and ends with a clean slate.
struct FScopedProviders
{
	FScopedProviders() { FFixProviderRegistry::Clear(); }
	~FScopedProviders() { FFixProviderRegistry::Clear(); }
};

} // namespace

void FUnrealOpenMcpFixProviderRegistrySpec::Define()
{
	Describe("Register / Find", [this]()
	{
		It("registers and finds a provider by FixId", [this]()
		{
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("safe_fix"), TEXT("rule"), TEXT("code")));

			TestTrue(TEXT("available contains safe_fix"),
				FFixProviderRegistry::AvailableFixIds().Contains(TEXT("safe_fix")));
			TestNotNull(TEXT("Find returns the provider"), FFixProviderRegistry::Find(TEXT("safe_fix")));
		});

		It("does not double-register a duplicate FixId", [this]()
		{
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("dup"), TEXT("rule"), TEXT("code")));
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("dup"), TEXT("other_rule"), TEXT("other_code")));

			TestEqual(TEXT("only one provider"), FFixProviderRegistry::AvailableFixIds().Num(), 1);
		});

		It("ignores a null provider", [this]()
		{
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(nullptr);
			TestEqual(TEXT("no providers"), FFixProviderRegistry::AvailableFixIds().Num(), 0);
		});

		It("returns nullptr for an unknown FixId", [this]()
		{
			FScopedProviders Guard;
			TestNull(TEXT("Find nullptr"), FFixProviderRegistry::Find(TEXT("does_not_exist")));
		});
	});

	Describe("TryGetFixInfo", [this]()
	{
		It("surfaces the real Safe flag for a safe provider", [this]()
		{
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("safe_fix"), TEXT("rule"), TEXT("code")));

			FString FixId;
			bool bSafe = true;
			const bool bOk = FFixProviderRegistry::TryGetFixInfo(TEXT("rule"), TEXT("code"), FixId, bSafe);

			TestTrue(TEXT("ok"), bOk);
			TestEqual(TEXT("fixId"), FixId, FString(TEXT("safe_fix")));
			TestTrue(TEXT("safe"), bSafe);
		});

		It("surfaces the real Safe flag for an unsafe provider", [this]()
		{
			// Regression guard: a previous Unity implementation hardwired
			// safe=true and masked unsafe fixes as auto-applyable. The flag
			// must come from Describe() so unsafe fixes (e.g.
			// relink_broken_guid) are advertised accurately.
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FUnsafeStubProvider>(TEXT("unsafe_fix"), TEXT("rule"), TEXT("code")));

			FString FixId;
			bool bSafe = true;
			const bool bOk = FFixProviderRegistry::TryGetFixInfo(TEXT("rule"), TEXT("code"), FixId, bSafe);

			TestTrue(TEXT("ok"), bOk);
			TestEqual(TEXT("fixId"), FixId, FString(TEXT("unsafe_fix")));
			TestFalse(TEXT("unsafe"), bSafe);
		});

		It("returns false when no provider matches", [this]()
		{
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("safe_fix"), TEXT("rule"), TEXT("code")));

			FString FixId;
			bool bSafe = true;
			TestFalse(TEXT("ok"),
				FFixProviderRegistry::TryGetFixInfo(TEXT("unmatched_rule"), TEXT("code"), FixId, bSafe));
		});

		It("returns false for empty inputs", [this]()
		{
			FScopedProviders Guard;
			FString FixId;
			bool bSafe = true;
			TestFalse(TEXT("empty rule"),
				FFixProviderRegistry::TryGetFixInfo(FString(), TEXT("code"), FixId, bSafe));
			TestFalse(TEXT("empty issueCode"),
				FFixProviderRegistry::TryGetFixInfo(TEXT("rule"), FString(), FixId, bSafe));
		});

#if WITH_EXCEPTIONS
		It("defaults Safe to false when Describe throws", [this]()
		{
			// The gate must never auto-apply something it cannot reason about.
			// If Describe throws, default Safe to false.
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FThrowingDescribeProvider>(TEXT("throwy_fix"), TEXT("rule"), TEXT("code")));

			FString FixId;
			bool bSafe = true;
			const bool bOk = FFixProviderRegistry::TryGetFixInfo(TEXT("rule"), TEXT("code"), FixId, bSafe);

			TestTrue(TEXT("ok"), bOk);
			TestEqual(TEXT("fixId"), FixId, FString(TEXT("throwy_fix")));
			TestFalse(TEXT("safe defaulted to false"), bSafe);
		});
#endif // WITH_EXCEPTIONS
	});

	Describe("FixesForIssue", [this]()
	{
		It("returns every matching provider for a canonical issue id", [this]()
		{
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FUnsafeStubProvider>(TEXT("fix_one"), TEXT("rule"), TEXT("code")));
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FUnsafeStubProvider>(TEXT("fix_two"), TEXT("rule"), TEXT("code")));
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FUnsafeStubProvider>(TEXT("unrelated"), TEXT("other_rule"), TEXT("code")));

			const FString IssueId = FIssueKey::Build(
				TEXT("rule"), EVerifySeverity::Error, TEXT("/Game/A.uasset"), TEXT("code"));
			const TArray<FString> Fixes = FFixProviderRegistry::FixesForIssue(IssueId);

			TestEqual(TEXT("two matches"), Fixes.Num(), 2);
			TestTrue(TEXT("has fix_one"), Fixes.Contains(TEXT("fix_one")));
			TestTrue(TEXT("has fix_two"), Fixes.Contains(TEXT("fix_two")));
		});

		It("returns empty for an unknown issue id", [this]()
		{
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("fix_one"), TEXT("rule"), TEXT("code")));

			const FString IssueId = FIssueKey::Build(
				TEXT("unmatched_rule"), EVerifySeverity::Error, TEXT("/Game/A.uasset"), TEXT("code"));
			TestEqual(TEXT("no matches"), FFixProviderRegistry::FixesForIssue(IssueId).Num(), 0);
		});

		It("returns empty for an empty issue id", [this]()
		{
			FScopedProviders Guard;
			TestEqual(TEXT("empty"), FFixProviderRegistry::FixesForIssue(FString()).Num(), 0);
		});
	});

	Describe("CandidatesForIssue", [this]()
	{
		It("returns one candidate per matching provider, each with its Safe flag", [this]()
		{
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("safe_one"), TEXT("rule"), TEXT("code")));
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FUnsafeStubProvider>(TEXT("unsafe_one"), TEXT("rule"), TEXT("code")));

			const TArray<FFixCandidate> Candidates =
				FFixProviderRegistry::CandidatesForIssue(TEXT("rule"), TEXT("code"));

			TestEqual(TEXT("two candidates"), Candidates.Num(), 2);

			// Find each candidate by FixId and assert its Safe flag — order is
			// registration order, but the spec asserts by lookup so it is
			// robust to re-ordering.
			bool bSawSafe = false;
			bool bSawUnsafe = false;
			for (const FFixCandidate& C : Candidates)
			{
				if (C.FixId == TEXT("safe_one"))
				{
					bSawSafe = true;
					TestTrue(TEXT("safe_one Safe"), C.bSafe);
				}
				else if (C.FixId == TEXT("unsafe_one"))
				{
					bSawUnsafe = true;
					TestFalse(TEXT("unsafe_one Safe"), C.bSafe);
				}
			}
			TestTrue(TEXT("saw safe_one"), bSawSafe);
			TestTrue(TEXT("saw unsafe_one"), bSawUnsafe);
		});

		It("returns empty when no provider matches", [this]()
		{
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("safe_one"), TEXT("rule"), TEXT("code")));

			TestEqual(TEXT("no candidates"),
				FFixProviderRegistry::CandidatesForIssue(TEXT("unmatched"), TEXT("code")).Num(), 0);
		});

		It("returns empty for empty inputs", [this]()
		{
			FScopedProviders Guard;
			TestEqual(TEXT("empty rule"),
				FFixProviderRegistry::CandidatesForIssue(FString(), TEXT("code")).Num(), 0);
			TestEqual(TEXT("empty code"),
				FFixProviderRegistry::CandidatesForIssue(TEXT("rule"), FString()).Num(), 0);
		});
	});

	Describe("AvailableFixIds", [this]()
	{
		It("lists every registered FixId", [this]()
		{
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("a"), TEXT("r1"), TEXT("c1")));
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("b"), TEXT("r2"), TEXT("c2")));

			const TArray<FString> Ids = FFixProviderRegistry::AvailableFixIds();

			TestEqual(TEXT("two ids"), Ids.Num(), 2);
			TestTrue(TEXT("has a"), Ids.Contains(TEXT("a")));
			TestTrue(TEXT("has b"), Ids.Contains(TEXT("b")));
		});

		It("returns empty after Clear", [this]()
		{
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("a"), TEXT("r1"), TEXT("c1")));
			FFixProviderRegistry::Clear();
			TestEqual(TEXT("empty after clear"), FFixProviderRegistry::AvailableFixIds().Num(), 0);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
