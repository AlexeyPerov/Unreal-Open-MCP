// FVerifyRunner — the scoped scan driver.
//
// Ported from Unity Open MCP packages/verify/Editor/Core/VerifyRunner.cs at
// copy fidelity, adapted to UE module lifecycle:
//   - Unity uses [InitializeOnLoadMethod] for RegisterDefaults; Unreal uses
//     module StartupModule + idempotent EnsureDefaultsRegistered(). Both the
//     verify module's StartupModule and the bridge's gate boot call the
//     latter so a standalone editor (no bridge) and a bridge-driven path
//     converge on the same registered rule set.
//   - Unity's runner is a static class; the Unreal port keeps the same
//     static surface so existing call sites transfer one-to-one.
//
// P3.1 scope: the runner shell only. RegisterDefaults() is a placeholder —
// the concrete rule families (broken_soft_references, missing_blueprint_parent,
// compile_error, content_path_hygiene) land in P3.2–P3.4 / P3.7. Until then
// the runner's rule list is empty and RunScoped returns an empty result.
//
// The runner swallows exceptions thrown from a rule's Scan() so one bad rule
// cannot abort a gate pass — the issue is logged and the remaining rules
// still run (Unity parity; pinned in VerifyRunnerTests).
#pragma once

#include "CoreMinimal.h"

#include "Core/CheckpointFingerprint.h"
#include "Core/IVerifyRule.h"
#include "Core/VerifyIssue.h"
#include "Core/VerifyResult.h"
#include "Core/VerifyRunMode.h"
#include "Core/VerifyScope.h"

/**
 * Scoped scan driver. Static surface mirroring Unity's VerifyRunner.
 */
class UNREALOPENMCPVERIFY_API FVerifyRunner
{
public:
	// Budget (ms) for the Checkpoint run mode. A Checkpoint pass that exceeds
	// it logs a warning so an operator notices a rule that grew too heavy
	// for the pre-mutation snapshot. Mirrors Unity's CheckpointBudgetMs.
	static constexpr int64 CheckpointBudgetMs = 2000;

	/**
	 * Idempotent: register the default rule families if the registry is
	 * empty. Called from the verify module's StartupModule and from the
	 * bridge gate boot (P3.5). P3.1 leaves the body empty — concrete rule
	 * families land in P3.2–P3.4 / P3.7.
	 */
	static void EnsureDefaultsRegistered();

	/** P3.1 placeholder for the concrete rule families. */
	static void RegisterDefaults();

	/** All currently registered rules (in registration order). */
	static const TArray<TUniquePtr<IVerifyRule>>& GetRules();

	/**
	 * Register an additional rule. No-op if a rule with the same Id is
	 * already registered.
	 */
	static void RegisterRule(TUniquePtr<IVerifyRule> Rule);

	/** Remove every registered rule (test helper). */
	static void ClearRules();

	/**
	 * Run the registered rules (or the requested subset) over the scope.
	 *
	 * @param Scope     paths / include-dependents bound.
	 * @param RuleIds   requested rule Ids. Empty → run all registered.
	 * @param Mode      Checkpoint (cheap, pre-mutation) / Validate (post-
	 *                  mutation, paths_hint-bounded) / Full (no mode hint).
	 * @return FVerifyResult with the issues, the Ids actually run, the
	 *         duration, and the unknown/available Id rosters.
	 */
	static FVerifyResult RunScoped(const FVerifyScope& Scope, const TArray<FString>& RuleIds, const EVerifyRunMode Mode);

	/**
	 * Create a checkpoint fingerprint — the pre-mutation snapshot the gate
	 * delta (P3.6) compares the post-mutation run against.
	 */
	static FCheckpointFingerprint CreateCheckpoint(const FVerifyScope& Scope, const TArray<FString>& RuleIds);
};
