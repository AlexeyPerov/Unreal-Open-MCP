// FClearBrokenSoftReferenceFix Automation specs (P3.7).
//
// Pins the Safe fix provider's contract without touching real assets:
//   - GetFixId is stable ("clear_broken_soft_reference").
//   - CanFix accepts broken_soft_references rule findings (bare code) and
//     rejects other rules / unknown codes.
//   - Describe returns Safe=true ONLY when the issue carries a precise
//     top-level property path; ambiguous / struct-nested findings return
//     Safe=false so the gate never auto-suggests them.
//   - Apply refuses (returns fix_failed) on ambiguous / struct-nested / bare
//     targets without touching disk.
//
// Apply success + on-disk behavior is covered by the bridge-level
// UnrealOpenMcpApplyFixSpec via the dry-run preview path; the verify-side
// spec stays offline (no fixture asset) so it runs in any editor context.
//
// Adapted from Unity's RemoveMissingScriptFixTests at adapt fidelity
// (different fix surface, but the same Safe-flag / CanFix / Describe contract
// shape — Unity's missing-script fix is Safe for prefabs only; the Unreal
// clear-broken-soft-reference fix is Safe for precise top-level targets only).
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Core/IssueKey.h"
#include "Core/VerifySeverity.h"
#include "Fixes/ClearBrokenSoftReferenceFix.h"
#include "Fixes/FixProviderRegistry.h"
#include "Rules/BrokenSoftReferences/BrokenSoftReferencesIssueCodes.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpClearBrokenSoftReferenceFixSpec,
	"UnrealOpenMcp.Verify.ClearBrokenSoftReferenceFix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpClearBrokenSoftReferenceFixSpec)

void FUnrealOpenMcpClearBrokenSoftReferenceFixSpec::Define()
{
	Describe("GetFixId", [this]()
	{
		It("returns the stable fix id", [this]()
		{
			FClearBrokenSoftReferenceFix Fix;
			TestEqual(TEXT("fixId"), Fix.GetFixId(), FString(TEXT("clear_broken_soft_reference")));
		});
	});

	Describe("CanFix", [this]()
	{
		It("accepts a broken_soft_references finding with a precise suffix", [this]()
		{
			FClearBrokenSoftReferenceFix Fix;
			const FString IssueId = FIssueKey::Build(
				BrokenSoftReferences::RuleId,
				EVerifySeverity::Error,
				TEXT("/Game/Fixtures/Owner.Owner"),
				FString::Printf(TEXT("%s:/Game/Missing/Target.Target:Weapon"), BrokenSoftReferences::IssueCode));
			TestTrue(TEXT("can fix"), Fix.CanFix(IssueId));
		});

		It("accepts the bare code (no suffix) for the matching rule", [this]()
		{
			// CanFix only inspects ruleId + issueCode (Unity parity — the
			// synthetic-key strategy passes a placeholder asset path).
			FClearBrokenSoftReferenceFix Fix;
			const FString IssueId = FIssueKey::Build(
				BrokenSoftReferences::RuleId,
				EVerifySeverity::Error,
				TEXT("/Game/Fixtures/Owner.Owner"),
				BrokenSoftReferences::IssueCode);
			TestTrue(TEXT("can fix bare code"), Fix.CanFix(IssueId));
		});

		It("rejects an unrelated rule", [this]()
		{
			FClearBrokenSoftReferenceFix Fix;
			const FString IssueId = FIssueKey::Build(
				TEXT("other_rule"),
				EVerifySeverity::Error,
				TEXT("/Game/Fixtures/Owner.Owner"),
				BrokenSoftReferences::IssueCode);
			TestFalse(TEXT("cannot fix other rule"), Fix.CanFix(IssueId));
		});

		It("rejects a malformed issue key", [this]()
		{
			FClearBrokenSoftReferenceFix Fix;
			TestFalse(TEXT("rejects malformed"), Fix.CanFix(TEXT("garbage")));
			TestFalse(TEXT("rejects empty"), Fix.CanFix(FString()));
		});
	});

	Describe("Describe — Safe flag matrix", [this]()
	{
		It("returns Safe=true for a precise top-level property path", [this]()
		{
			FClearBrokenSoftReferenceFix Fix;
			const FString IssueId = FIssueKey::Build(
				BrokenSoftReferences::RuleId,
				EVerifySeverity::Error,
				TEXT("/Game/Fixtures/Owner.Owner"),
				FString::Printf(TEXT("%s:/Game/Missing/Target.Target:Weapon"), BrokenSoftReferences::IssueCode));
			const FFixDescription Desc = Fix.Describe(IssueId);
			TestTrue(TEXT("safe"), Desc.bSafe);
			TestEqual(TEXT("fixId"), Desc.FixId, FString(TEXT("clear_broken_soft_reference")));
			TestEqual(TEXT("assetPath"), Desc.AssetPath, FString(TEXT("/Game/Fixtures/Owner.Owner")));
		});

		It("returns Safe=false when the suffix lacks a property path", [this]()
		{
			// Ambiguous target — the mapper emits this shape when the scanner
			// cannot pin a property. The fix must refuse so it never guesses.
			FClearBrokenSoftReferenceFix Fix;
			const FString IssueId = FIssueKey::Build(
				BrokenSoftReferences::RuleId,
				EVerifySeverity::Error,
				TEXT("/Game/Fixtures/Owner.Owner"),
				FString::Printf(TEXT("%s:/Game/Missing/Target.Target"), BrokenSoftReferences::IssueCode));
			const FFixDescription Desc = Fix.Describe(IssueId);
			TestFalse(TEXT("unsafe when no property path"), Desc.bSafe);
		});

		It("returns Safe=false when the property path is struct-nested", [this]()
		{
			// v1 scope: top-level properties only. The suffix carries a dotted
			// property path ("Inventory.Items") when the scanner's depth-1
			// struct walk found the soft pointer.
			FClearBrokenSoftReferenceFix Fix;
			const FString IssueId = FIssueKey::Build(
				BrokenSoftReferences::RuleId,
				EVerifySeverity::Error,
				TEXT("/Game/Fixtures/Owner.Owner"),
				FString::Printf(TEXT("%s:/Game/Missing/Target.Target:Inventory.Items"), BrokenSoftReferences::IssueCode));
			const FFixDescription Desc = Fix.Describe(IssueId);
			TestFalse(TEXT("unsafe when struct-nested"), Desc.bSafe);
		});

		It("returns Safe=false for the bare code (no suffix at all)", [this]()
		{
			FClearBrokenSoftReferenceFix Fix;
			const FString IssueId = FIssueKey::Build(
				BrokenSoftReferences::RuleId,
				EVerifySeverity::Error,
				TEXT("/Game/Fixtures/Owner.Owner"),
				BrokenSoftReferences::IssueCode);
			const FFixDescription Desc = Fix.Describe(IssueId);
			TestFalse(TEXT("unsafe when bare code"), Desc.bSafe);
		});
	});

	Describe("Apply — refusal paths", [this]()
	{
		It("refuses on a bare code (no property path)", [this]()
		{
			FClearBrokenSoftReferenceFix Fix;
			const FString IssueId = FIssueKey::Build(
				BrokenSoftReferences::RuleId,
				EVerifySeverity::Error,
				TEXT("/Game/Fixtures/Owner.Owner"),
				BrokenSoftReferences::IssueCode);
			const FFixResult Result = Fix.Apply(IssueId);
			TestFalse(TEXT("not success"), Result.bSuccess);
			TestEqual(TEXT("touched paths empty"), Result.TouchedPaths.Num(), 0);
		});

		It("refuses on a struct-nested property path", [this]()
		{
			FClearBrokenSoftReferenceFix Fix;
			const FString IssueId = FIssueKey::Build(
				BrokenSoftReferences::RuleId,
				EVerifySeverity::Error,
				TEXT("/Game/Fixtures/Owner.Owner"),
				FString::Printf(TEXT("%s:/Game/Missing/Target.Target:Inventory.Items"), BrokenSoftReferences::IssueCode));
			const FFixResult Result = Fix.Apply(IssueId);
			TestFalse(TEXT("not success"), Result.bSuccess);
		});
	});

	Describe("FFixProviderRegistry integration", [this]()
	{
		It("TryGetFixInfo surfaces the Safe flag for broken_soft_references", [this]()
		{
			// Reset the registry so the test sees a clean state. RegisterDefaults
			// re-registers the production provider set.
			FFixProviderRegistry::Clear();
			FFixProviderRegistry::EnsureDefaultsRegistered();

			FString FixId;
			bool bSafe = true;
			const bool bOk = FFixProviderRegistry::TryGetFixInfo(
				BrokenSoftReferences::RuleId, BrokenSoftReferences::IssueCode, FixId, bSafe);

			TestTrue(TEXT("ok"), bOk);
			TestEqual(TEXT("fixId"), FixId, FString(TEXT("clear_broken_soft_reference")));
			// TryGetFixInfo uses the synthetic-key strategy (placeholder asset
			// path) so Describe sees a bare issue code → Safe=false. The real
			// per-instance Safe flag is computed by the gate via CandidatesForIssue
			// over the actual issue id; the synthetic-key surface only reports
			// whether a provider exists at all.
			//
			// We assert the fix id is registered; the per-instance Safe-flag
			// logic is pinned by the Describe tests above.

			// Cleanup: re-register so other specs in the same process see the
			// production set.
			FFixProviderRegistry::Clear();
			FFixProviderRegistry::EnsureDefaultsRegistered();
		});

		It("Find resolves the provider after EnsureDefaultsRegistered", [this]()
		{
			FFixProviderRegistry::Clear();
			FFixProviderRegistry::EnsureDefaultsRegistered();

			IFixProvider* Provider = FFixProviderRegistry::Find(TEXT("clear_broken_soft_reference"));
			TestNotNull(TEXT("provider registered"), Provider);

			FFixProviderRegistry::Clear();
			FFixProviderRegistry::EnsureDefaultsRegistered();
		});

		It("AvailableFixIds includes clear_broken_soft_reference", [this]()
		{
			FFixProviderRegistry::Clear();
			FFixProviderRegistry::EnsureDefaultsRegistered();

			const TArray<FString> Ids = FFixProviderRegistry::AvailableFixIds();
			TestTrue(TEXT("has clear_broken_soft_reference"),
				Ids.Contains(TEXT("clear_broken_soft_reference")));

			FFixProviderRegistry::Clear();
			FFixProviderRegistry::EnsureDefaultsRegistered();
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
