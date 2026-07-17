// FFixProviderRegistry — the lookup the gate and apply_fix dispatch through.
//
// Ported from Unity Open MCP packages/verify/Editor/Fixes/FixProviderRegistry.cs
// at copy fidelity. The registry resolves providers deterministically and
// reports the Safe flag accurately — unsafe providers (e.g.
// relink_broken_guid) must NOT be advertised as auto-applyable. The previous
// Unity implementation hardwired Safe=true and masked every fix as safe;
// the tests pin the real flag here so the regression cannot recur.
//
// Registration is module-driven (the verify module's StartupModule calls
// EnsureDefaultsRegistered) and idempotent. P3.1 ships the registry shell
// only — the concrete fix providers (remove_missing_script, relink_broken_guid,
// remove_orphan_meta, fix_duplicate_guid, reassign_missing_texture,
// reassign_missing_shader) land in P3.7.
//
// CanFix(testKey) only inspects ruleId + issueCode, so TryGetFixInfo /
// CandidatesForIssue build a synthetic key carrying a placeholder asset path
// and let each provider decide. The synthetic key matches the same set of
// providers that would respond to a real issue id (Unity parity).
#pragma once

#include "CoreMinimal.h"

#include "Fixes/FixContracts.h"

/**
 * Static lookup surface for fix providers. Mirrors Unity's
 * FixProviderRegistry static class.
 */
class UNREALOPENMCPVERIFY_API FFixProviderRegistry
{
public:
	/**
	 * Idempotent: register the default fix providers if the registry is
	 * empty. Called from the verify module's StartupModule. P3.1 leaves the
	 * body empty — concrete providers land in P3.7.
	 */
	static void EnsureDefaultsRegistered();

	/** P3.1 placeholder for the concrete fix providers. */
	static void RegisterDefaults();

	/** Register an additional provider. No-op on FixId collision. */
	static void RegisterProvider(TUniquePtr<IFixProvider> Provider);

	/** Find a provider by FixId, or nullptr. */
	static IFixProvider* Find(const FString& FixId);

	/**
	 * First fix matching a rule + issueCode pair plus the provider's real
	 * Safe flag. Safe is taken from Describe() so unsafe providers are
	 * surfaced accurately; if Describe throws, default to unsafe so the gate
	 * never auto-applies something it cannot reason about.
	 *
	 * @return true when a provider matched; OutFixId + OutSafe carry the info.
	 */
	static bool TryGetFixInfo(const FString& RuleId, const FString& IssueCode, FString& OutFixId, bool& OutSafe);

	/**
	 * Every fix that can resolve a given issue id (canonical issue key).
	 * Unlike TryGetFixInfo (first match) this returns the full set so
	 * apply_fix can advertise all available fixes per issue.
	 */
	static TArray<FString> FixesForIssue(const FString& IssueId);

	/**
	 * Every fix candidate for a rule + issue pair, each with its real Safe
	 * flag from Describe(). Used by scan_paths / validate_edit to emit a
	 * fixCandidates[] block so agents see safe vs unsafe options up front.
	 */
	static TArray<FFixCandidate> CandidatesForIssue(const FString& RuleId, const FString& IssueCode);

	/** Every registered FixId. */
	static TArray<FString> AvailableFixIds();

	/** Remove every registered provider (test helper). */
	static void Clear();

private:
	// Build the synthetic issue key the providers' CanFix sees. Providers
	// only inspect ruleId + issueCode, so the placeholder asset path matches
	// the same set of providers that would respond to a real issue id.
	static FString SyntheticKey(const FString& RuleId, const FString& IssueCode)
	{
		return FString::Printf(TEXT("%s|ERROR|__test__.uasset|%s"), *RuleId, *IssueCode);
	}
};
