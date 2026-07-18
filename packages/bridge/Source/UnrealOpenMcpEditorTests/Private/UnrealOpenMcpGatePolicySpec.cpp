// Gate policy Automation specs (P3.5).
//
// Adapts Unity Open MCP's GatePolicyTests
// (packages/bridge/Tests/Editor/Gate/GatePolicyTests.cs) to Unreal. Pins the
// gate dispatch contract:
//   - ResolveOutcome decision matrix (Failed/Warned/Passed by mode + delta).
//   - ParseMode wire token → enum mapping (case-sensitive; unknown → Enforce).
//   - Execute: paths_hint mandatory (empty hint skips the gate like Off).
//   - Execute: Off mode skips the gate (no checkpoint, no validate).
//   - Execute: read-only outcome is Skipped (success) / Failed (mutation error).
//   - Execute: Enforce + new Errors → Failed (gateFailed true).
//   - Execute: Warn + new Errors → Warned (mutation committed, gateFailed false).
//   - Execute: clean delta → Passed.
//   - GenerateAgentNextSteps: actionable guidance per outcome.
//   - No double-gate: the registry routes each mutating tool through exactly
//     one GatePolicy.Execute (the dispatch path's single chokepoint).
//
// Stub rules emit deterministic issues over a fixture content path so the gate
// delta has a known shape. The fixture path does not need to exist on disk —
// the verify runner's stub rules do not consult the asset registry.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Bridge/UnrealOpenMcpBridgeEnvelope.h"
#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Gate/UnrealOpenMcpGatePolicy.h"

#include "Core/IVerifyRule.h"
#include "Core/IssueKey.h"
#include "Core/VerifyIssue.h"
#include "Core/VerifyRunMode.h"
#include "Core/VerifyRunner.h"
#include "Core/VerifyScope.h"
#include "Core/VerifySeverity.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpGatePolicySpec,
	"UnrealOpenMcp.Bridge.GatePolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpGatePolicySpec)

namespace
{
	// Stub rule that emits exactly one Error per scan. Used to pin the
	// Enforce-fails / Warn-warns decision matrix.
	class FErrorRule : public IVerifyRule
	{
	public:
		FString Id;
		explicit FErrorRule(FString InId) : Id(MoveTemp(InId)) {}
		virtual FString GetId() const override { return Id; }
		virtual void Scan(const FVerifyScope& Scope, const EVerifyRunMode Mode, TArray<FVerifyIssue>& Sink) const override
		{
			Sink.Emplace(Id, EVerifySeverity::Error,
				TEXT("/Game/Fixtures/GatePolicyTest.uasset"), TEXT("gate_policy_error"), TEXT("gate policy spec error"));
		}
	};

	// Stub rule that emits one Warning per scan. Used to pin the
	// warnings-only path (Passed under Enforce, Warned under Warn).
	class FWarningRule : public IVerifyRule
	{
	public:
		FString Id;
		explicit FWarningRule(FString InId) : Id(MoveTemp(InId)) {}
		virtual FString GetId() const override { return Id; }
		virtual void Scan(const FVerifyScope& Scope, const EVerifyRunMode Mode, TArray<FVerifyIssue>& Sink) const override
		{
			Sink.Emplace(Id, EVerifySeverity::Warning,
				TEXT("/Game/Fixtures/GatePolicyTest.uasset"), TEXT("gate_policy_warn"), TEXT("gate policy spec warning"));
		}
	};

	// RAII guard: clear the rule registry on construct / destruct so each spec
	// sees a clean slate. The registry is process-global static state.
	struct FScopedRules
	{
		FScopedRules() { FVerifyRunner::ClearRules(); }
		~FScopedRules() { FVerifyRunner::ClearRules(); }
	};

	FUnrealOpenMcpGateDelta MakeDelta(int32 Errors = 0, int32 Warnings = 0, int32 ResolvedErrors = 0)
	{
		FUnrealOpenMcpGateDelta D;
		D.NewErrors = Errors;
		D.NewWarnings = Warnings;
		D.ResolvedErrors = ResolvedErrors;
		return D;
	}

	TArray<FString> Paths()
	{
		TArray<FString> P;
		P.Add(TEXT("/Game/Fixtures/GatePolicyTest.uasset"));
		return P;
	}

	TArray<FString> EmptyPaths()
	{
		return {};
	}

	FUnrealOpenMcpToolDispatchResult OkMutation()
	{
		return FUnrealOpenMcpToolDispatchResult::Ok(TEXT("{\"touched\":[\"/Game/Fixtures/GatePolicyTest.uasset\"]}"));
	}

	FUnrealOpenMcpToolDispatchResult FailMutation()
	{
		return FUnrealOpenMcpToolDispatchResult::Fail(TEXT("mutation_failed"), TEXT("mutation blew up"));
	}
} // namespace

void FUnrealOpenMcpGatePolicySpec::Define()
{
	Describe("ResolveOutcome — decision matrix", [this]()
	{
		It("new Errors + Enforce → Failed (gateFailed)", [this]()
		{
			EUnrealOpenMcpGateOutcome Outcome;
			bool bGateFailed = false;
			FUnrealOpenMcpGatePolicy::ResolveOutcome(
				EUnrealOpenMcpGateMode::Enforce, MakeDelta(/*Errors=*/2), Outcome, bGateFailed);
			TestEqual(TEXT("Failed"), Outcome, EUnrealOpenMcpGateOutcome::Failed);
			TestTrue(TEXT("gateFailed"), bGateFailed);
		});

		It("new Errors + Warn → Warned (gateFailed false)", [this]()
		{
			EUnrealOpenMcpGateOutcome Outcome;
			bool bGateFailed = false;
			FUnrealOpenMcpGatePolicy::ResolveOutcome(
				EUnrealOpenMcpGateMode::Warn, MakeDelta(/*Errors=*/2), Outcome, bGateFailed);
			TestEqual(TEXT("Warned"), Outcome, EUnrealOpenMcpGateOutcome::Warned);
			TestFalse(TEXT("not gateFailed"), bGateFailed);
		});

		It("new Warnings + Warn → Warned", [this]()
		{
			EUnrealOpenMcpGateOutcome Outcome;
			bool bGateFailed = false;
			FUnrealOpenMcpGatePolicy::ResolveOutcome(
				EUnrealOpenMcpGateMode::Warn, MakeDelta(/*Warnings=*/3), Outcome, bGateFailed);
			TestEqual(TEXT("Warned"), Outcome, EUnrealOpenMcpGateOutcome::Warned);
			TestFalse(TEXT("not gateFailed"), bGateFailed);
		});

		It("new Warnings + Enforce → Passed", [this]()
		{
			// Enforce only blocks on Errors; warnings alone pass.
			EUnrealOpenMcpGateOutcome Outcome;
			bool bGateFailed = false;
			FUnrealOpenMcpGatePolicy::ResolveOutcome(
				EUnrealOpenMcpGateMode::Enforce, MakeDelta(/*Warnings=*/3), Outcome, bGateFailed);
			TestEqual(TEXT("Passed"), Outcome, EUnrealOpenMcpGateOutcome::Passed);
			TestFalse(TEXT("not gateFailed"), bGateFailed);
		});

		It("clean delta → Passed", [this]()
		{
			EUnrealOpenMcpGateOutcome Outcome;
			bool bGateFailed = false;
			FUnrealOpenMcpGatePolicy::ResolveOutcome(
				EUnrealOpenMcpGateMode::Enforce, MakeDelta(), Outcome, bGateFailed);
			TestEqual(TEXT("Passed"), Outcome, EUnrealOpenMcpGateOutcome::Passed);
			TestFalse(TEXT("not gateFailed"), bGateFailed);
		});

		It("Errors take priority over Warnings", [this]()
		{
			EUnrealOpenMcpGateOutcome Outcome;
			bool bGateFailed = false;
			FUnrealOpenMcpGatePolicy::ResolveOutcome(
				EUnrealOpenMcpGateMode::Enforce, MakeDelta(/*Errors=*/1, /*Warnings=*/5), Outcome, bGateFailed);
			TestEqual(TEXT("Failed wins"), Outcome, EUnrealOpenMcpGateOutcome::Failed);
		});

		It("never returns ValidateScanFailed", [this]()
		{
			// ValidateScanFailed is set ONLY by the validate-exception catch in
			// Execute — ResolveOutcome must never produce it (parity invariant).
			for (EUnrealOpenMcpGateMode Mode : {
				EUnrealOpenMcpGateMode::Enforce,
				EUnrealOpenMcpGateMode::Warn,
				EUnrealOpenMcpGateMode::Off })
			{
				EUnrealOpenMcpGateOutcome Outcome;
				bool bGateFailed = false;
				FUnrealOpenMcpGatePolicy::ResolveOutcome(
					Mode, MakeDelta(/*Errors=*/5, /*Warnings=*/5), Outcome, bGateFailed);
				TestNotEqual(
					FString::Printf(TEXT("mode %d: not ValidateScanFailed"), static_cast<int32>(Mode)),
					Outcome, EUnrealOpenMcpGateOutcome::ValidateScanFailed);
			}
		});
	});

	Describe("ParseMode — wire token → enum", [this]()
	{
		It("maps warn / off / enforce", [this]()
		{
			TestEqual(TEXT("warn"), FUnrealOpenMcpGatePolicy::ParseMode(TEXT("warn")), EUnrealOpenMcpGateMode::Warn);
			TestEqual(TEXT("off"), FUnrealOpenMcpGatePolicy::ParseMode(TEXT("off")), EUnrealOpenMcpGateMode::Off);
			TestEqual(TEXT("enforce"), FUnrealOpenMcpGatePolicy::ParseMode(TEXT("enforce")), EUnrealOpenMcpGateMode::Enforce);
		});

		It("falls back to Enforce for empty / unknown / wrong-case", [this]()
		{
			TestEqual(TEXT("empty"), FUnrealOpenMcpGatePolicy::ParseMode(TEXT("")), EUnrealOpenMcpGateMode::Enforce);
			TestEqual(TEXT("garbage"), FUnrealOpenMcpGatePolicy::ParseMode(TEXT("garbage")), EUnrealOpenMcpGateMode::Enforce);
			// Case-sensitive — WARN is unknown, not Warn.
			TestEqual(TEXT("WARN"), FUnrealOpenMcpGatePolicy::ParseMode(TEXT("WARN")), EUnrealOpenMcpGateMode::Enforce);
		});
	});

	Describe("OutcomeToken — enum → wire token", [this]()
	{
		It("emits the canonical wire tokens", [this]()
		{
			TestEqual(TEXT("Passed"), FString(FUnrealOpenMcpGatePolicy::OutcomeToken(EUnrealOpenMcpGateOutcome::Passed)), FString(TEXT("passed")));
			TestEqual(TEXT("Warned"), FString(FUnrealOpenMcpGatePolicy::OutcomeToken(EUnrealOpenMcpGateOutcome::Warned)), FString(TEXT("warned")));
			TestEqual(TEXT("Failed"), FString(FUnrealOpenMcpGatePolicy::OutcomeToken(EUnrealOpenMcpGateOutcome::Failed)), FString(TEXT("failed")));
			TestEqual(TEXT("Skipped"), FString(FUnrealOpenMcpGatePolicy::OutcomeToken(EUnrealOpenMcpGateOutcome::Skipped)), FString(TEXT("skipped")));
			TestEqual(TEXT("ValidateScanFailed"), FString(FUnrealOpenMcpGatePolicy::OutcomeToken(EUnrealOpenMcpGateOutcome::ValidateScanFailed)), FString(TEXT("validate_scan_failed")));
		});

		It("ValidateScanFailed is distinct from every other outcome", [this]()
		{
			// Parity invariant — the validate-exception path must not conflate
			// with a real delta failure.
			const FString Token = FUnrealOpenMcpGatePolicy::OutcomeToken(EUnrealOpenMcpGateOutcome::ValidateScanFailed);
			TestNotEqual(TEXT("!= passed"), Token, FString(FUnrealOpenMcpGatePolicy::OutcomeToken(EUnrealOpenMcpGateOutcome::Passed)));
			TestNotEqual(TEXT("!= warned"), Token, FString(FUnrealOpenMcpGatePolicy::OutcomeToken(EUnrealOpenMcpGateOutcome::Warned)));
			TestNotEqual(TEXT("!= failed"), Token, FString(FUnrealOpenMcpGatePolicy::OutcomeToken(EUnrealOpenMcpGateOutcome::Failed)));
			TestNotEqual(TEXT("!= skipped"), Token, FString(FUnrealOpenMcpGatePolicy::OutcomeToken(EUnrealOpenMcpGateOutcome::Skipped)));
		});
	});

	Describe("Execute — gate skipped paths", [this]()
	{
		It("Off mode skips the gate and surfaces Skipped on success", [this]()
		{
			FScopedRules Guard;
			bool bMutationRan = false;
			const FUnrealOpenMcpGateDispatchResult R = FUnrealOpenMcpGatePolicy::Execute(
				EUnrealOpenMcpGateMode::Off, Paths(),
				[&]() -> FUnrealOpenMcpToolDispatchResult
				{
					bMutationRan = true;
					return OkMutation();
				});
			TestTrue(TEXT("mutation ran"), bMutationRan);
			TestFalse(TEXT("gate not ran"), R.bGateRan);
			TestEqual(TEXT("Skipped"), R.Outcome, EUnrealOpenMcpGateOutcome::Skipped);
			TestFalse(TEXT("not gateFailed"), R.bGateFailed);
			TestTrue(TEXT("mutation ok"), R.Mutation.bOk);
		});

		It("Off mode surfaces Failed when the mutation fails", [this]()
		{
			FScopedRules Guard;
			const FUnrealOpenMcpGateDispatchResult R = FUnrealOpenMcpGatePolicy::Execute(
				EUnrealOpenMcpGateMode::Off, Paths(),
				[]() -> FUnrealOpenMcpToolDispatchResult
				{
					return FailMutation();
				});
			TestFalse(TEXT("gate not ran"), R.bGateRan);
			TestEqual(TEXT("Failed"), R.Outcome, EUnrealOpenMcpGateOutcome::Failed);
			TestTrue(TEXT("gateFailed"), R.bGateFailed);
		});

		It("empty paths_hint skips the gate (no whole-project fallback)", [this]()
		{
			// Even under Enforce, an empty hint must NOT fall back to a whole-
			// project scan. The dispatch policy's paths_hint_required check
			// fires before Execute ever runs; this spec pins the in-Execute
			// behavior so the gate stays honest if a caller smuggles an empty
			// hint through.
			FScopedRules Guard;
			const FUnrealOpenMcpGateDispatchResult R = FUnrealOpenMcpGatePolicy::Execute(
				EUnrealOpenMcpGateMode::Enforce, EmptyPaths(),
				[]() -> FUnrealOpenMcpToolDispatchResult
				{
					return OkMutation();
				});
			TestFalse(TEXT("gate not ran"), R.bGateRan);
			TestEqual(TEXT("Skipped"), R.Outcome, EUnrealOpenMcpGateOutcome::Skipped);
		});
	});

	Describe("Execute — gate run paths", [this]()
	{
		// Use LatentIt for any spec that may take >100ms (the verify runner
		// loads packages; stub rules do not, but the gate's checkpoint +
		// validate cycle still touches the registry). The non-latent form is
		// fine for stub-rule specs that complete in microseconds.

		It("Enforce + clean delta → Passed", [this]()
		{
			FScopedRules Guard;
			// No rules registered → empty Issues → clean delta → Passed.
			const FUnrealOpenMcpGateDispatchResult R = FUnrealOpenMcpGatePolicy::Execute(
				EUnrealOpenMcpGateMode::Enforce, Paths(),
				[]() -> FUnrealOpenMcpToolDispatchResult
				{
					return OkMutation();
				});
			TestTrue(TEXT("gate ran"), R.bGateRan);
			TestEqual(TEXT("Passed"), R.Outcome, EUnrealOpenMcpGateOutcome::Passed);
			TestFalse(TEXT("not gateFailed"), R.bGateFailed);
			TestTrue(TEXT("mutation ok"), R.Mutation.bOk);
			TestTrue(TEXT("checkpointId set"), !R.CheckpointId.IsEmpty());
			TestTrue(TEXT("delta set"), R.Delta.IsSet());
			if (R.Delta.IsSet())
			{
				TestEqual(TEXT("no new errors"), R.Delta->NewErrors, 0);
			}
		});

		It("Enforce + new Errors → Failed", [this]()
		{
			// Install a rule that emits an Error on every scan. The checkpoint
			// captures the same error (the rule fires pre-mutation too), so the
			// delta needs the rule to fire ONLY post-mutation. The simplest
			// setup: register the rule mid-flight (between checkpoint and
			// validate). For this spec, we simulate "new error post-mutation"
			// by registering the rule after the checkpoint — i.e. inside the
			// mutation callback, before validate runs.
			FScopedRules Guard;
			const FUnrealOpenMcpGateDispatchResult R = FUnrealOpenMcpGatePolicy::Execute(
				EUnrealOpenMcpGateMode::Enforce, Paths(),
				[]() -> FUnrealOpenMcpToolDispatchResult
				{
					// Register the error-emitting rule as the "side effect" of
					// the mutation so the post-mutation validate sees an error
					// the pre-mutation checkpoint did not.
					FVerifyRunner::RegisterRule(MakeUnique<FErrorRule>(TEXT("gate_policy_error_rule")));
					return OkMutation();
				});
			TestTrue(TEXT("gate ran"), R.bGateRan);
			TestEqual(TEXT("Failed"), R.Outcome, EUnrealOpenMcpGateOutcome::Failed);
			TestTrue(TEXT("gateFailed"), R.bGateFailed);
			TestTrue(TEXT("delta has new errors"), R.Delta.IsSet() && R.Delta->NewErrors > 0);
		});

		It("Warn + new Errors → Warned (mutation committed)", [this]()
		{
			FScopedRules Guard;
			const FUnrealOpenMcpGateDispatchResult R = FUnrealOpenMcpGatePolicy::Execute(
				EUnrealOpenMcpGateMode::Warn, Paths(),
				[]() -> FUnrealOpenMcpToolDispatchResult
				{
					FVerifyRunner::RegisterRule(MakeUnique<FErrorRule>(TEXT("gate_policy_error_rule")));
					return OkMutation();
				});
			TestTrue(TEXT("gate ran"), R.bGateRan);
			TestEqual(TEXT("Warned"), R.Outcome, EUnrealOpenMcpGateOutcome::Warned);
			// Mutation committed despite the new errors — Warn never rolls back.
			TestTrue(TEXT("mutation ok"), R.Mutation.bOk);
			TestFalse(TEXT("not gateFailed"), R.bGateFailed);
		});

		It("Enforce + new Warnings only → Passed", [this]()
		{
			FScopedRules Guard;
			const FUnrealOpenMcpGateDispatchResult R = FUnrealOpenMcpGatePolicy::Execute(
				EUnrealOpenMcpGateMode::Enforce, Paths(),
				[]() -> FUnrealOpenMcpToolDispatchResult
				{
					FVerifyRunner::RegisterRule(MakeUnique<FWarningRule>(TEXT("gate_policy_warn_rule")));
					return OkMutation();
				});
			TestTrue(TEXT("gate ran"), R.bGateRan);
			TestEqual(TEXT("Passed"), R.Outcome, EUnrealOpenMcpGateOutcome::Passed);
			TestTrue(TEXT("delta has new warnings"), R.Delta.IsSet() && R.Delta->NewWarnings > 0);
		});

		It("mutation failure short-circuits to Failed with no delta", [this]()
		{
			FScopedRules Guard;
			const FUnrealOpenMcpGateDispatchResult R = FUnrealOpenMcpGatePolicy::Execute(
				EUnrealOpenMcpGateMode::Enforce, Paths(),
				[]() -> FUnrealOpenMcpToolDispatchResult
				{
					return FailMutation();
				});
			TestTrue(TEXT("gate ran"), R.bGateRan);
			TestEqual(TEXT("Failed"), R.Outcome, EUnrealOpenMcpGateOutcome::Failed);
			TestTrue(TEXT("gateFailed"), R.bGateFailed);
			TestFalse(TEXT("no delta"), R.Delta.IsSet());
			// agentNextSteps must explain the mutation failure.
			TestTrue(TEXT("next steps non-empty"), R.AgentNextSteps.Num() > 0);
		});
	});

	Describe("GenerateAgentNextSteps — actionable guidance", [this]()
	{
		It("Failed mentions error count + validate_edit", [this]()
		{
			FUnrealOpenMcpGateDelta D = MakeDelta(/*Errors=*/2);
			D.NewIssueKeys.Add(TEXT("broken_soft_references|ERROR|/Game/Fixtures/A.uasset|broken_soft_reference"));
			const TArray<FString> Steps = FUnrealOpenMcpGatePolicy::GenerateAgentNextSteps(D, EUnrealOpenMcpGateOutcome::Failed);
			TestTrue(TEXT("non-empty"), Steps.Num() > 0);
			FString Joined = FString::Join(Steps, TEXT("\n"));
			TestTrue(TEXT("error count"), Joined.Contains(TEXT("2 new error(s)")));
			TestTrue(TEXT("mentions validate_edit"), Joined.Contains(TEXT("validate_edit")));
		});

		It("Passed with resolved errors notes the resolution", [this]()
		{
			const TArray<FString> Steps = FUnrealOpenMcpGatePolicy::GenerateAgentNextSteps(
				MakeDelta(/*ResolvedErrors=*/3), EUnrealOpenMcpGateOutcome::Passed);
			FString Joined = FString::Join(Steps, TEXT("\n"));
			TestTrue(TEXT("mentions resolution"), Joined.Contains(TEXT("3 previously reported error(s) resolved")));
		});

		It("Passed clean reports no new issues", [this]()
		{
			const TArray<FString> Steps = FUnrealOpenMcpGatePolicy::GenerateAgentNextSteps(
				MakeDelta(), EUnrealOpenMcpGateOutcome::Passed);
			FString Joined = FString::Join(Steps, TEXT("\n"));
			TestTrue(TEXT("no new issues"), Joined.Contains(TEXT("no new issues detected")));
		});

		It("Warned with warnings-only suggests review", [this]()
		{
			const TArray<FString> Steps = FUnrealOpenMcpGatePolicy::GenerateAgentNextSteps(
				MakeDelta(/*Warnings=*/4), EUnrealOpenMcpGateOutcome::Warned);
			FString Joined = FString::Join(Steps, TEXT("\n"));
			TestTrue(TEXT("warning count"), Joined.Contains(TEXT("4 new warning(s)")));
			TestTrue(TEXT("mentions validate_edit"), Joined.Contains(TEXT("validate_edit")));
		});

		It("Failed with malformed issue key does not crash", [this]()
		{
			FUnrealOpenMcpGateDelta D = MakeDelta(/*Errors=*/1);
			D.NewIssueKeys.Add(TEXT("garbage"));
			const TArray<FString> Steps = FUnrealOpenMcpGatePolicy::GenerateAgentNextSteps(D, EUnrealOpenMcpGateOutcome::Failed);
			TestTrue(TEXT("non-empty"), Steps.Num() > 0);
			FString Joined = FString::Join(Steps, TEXT("\n"));
			TestTrue(TEXT("fallback review hint"), Joined.Contains(TEXT("Review the affected asset")));
		});
	});

	Describe("Registry + dispatch — no double-gate", [this]()
	{
		// The single-chokepoint contract: a mutating tool registered with
		// Mutating metadata routes through GatePolicy.Execute exactly once.
		// A read-only tool registered with the ReadOnly shorthand never enters
		// the gate. This spec exercises the registry metadata API; the
		// HTTP-layer wiring is covered by the dispatch spec.
		It("mutating tool is flagged mutating + Enforce default", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			Registry.Register(
				TEXT("unreal_open_mcp_test_mutator"),
				[](const FString&) -> FUnrealOpenMcpToolDispatchResult
				{
					return FUnrealOpenMcpToolDispatchResult::Ok();
				},
				FUnrealOpenMcpToolMetadata::Mutating());

			FUnrealOpenMcpToolMetadata M;
			TestTrue(TEXT("metadata resolves"), Registry.TryGetMetadata(TEXT("unreal_open_mcp_test_mutator"), M));
			TestTrue(TEXT("mutating"), M.bIsMutating);
			TestEqual(TEXT("Enforce default"), M.DefaultGate, EUnrealOpenMcpGateMode::Enforce);
		});

		It("read-only shorthand defaults to non-mutating + Off", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			Registry.Register(
				TEXT("unreal_open_mcp_test_reader"),
				[](const FString&) -> FUnrealOpenMcpToolDispatchResult
				{
					return FUnrealOpenMcpToolDispatchResult::Ok();
				});

			FUnrealOpenMcpToolMetadata M;
			TestTrue(TEXT("metadata resolves"), Registry.TryGetMetadata(TEXT("unreal_open_mcp_test_reader"), M));
			TestFalse(TEXT("not mutating"), M.bIsMutating);
			TestEqual(TEXT("Off default"), M.DefaultGate, EUnrealOpenMcpGateMode::Off);
		});

		It("metadata miss returns false (default ReadOnly metadata is caller-supplied)", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpToolMetadata M = FUnrealOpenMcpToolMetadata::Mutating();
			TestFalse(TEXT("miss"), Registry.TryGetMetadata(TEXT("unreal_open_mcp_nope"), M));
			// Out parameter is unchanged on a miss.
			TestTrue(TEXT("unchanged"), M.bIsMutating);
		});
	});

	Describe("Envelope — widened gate summary", [this]()
	{
		It("BuildSuccessWithGate emits ok+result+gate", [this]()
		{
			FUnrealOpenMcpGateDispatchResult G;
			G.Mutation = OkMutation();
			G.bGateRan = true;
			G.Outcome = EUnrealOpenMcpGateOutcome::Passed;
			G.bGateFailed = false;
			G.CheckpointId = TEXT("cp_abcdef");
			G.Delta = MakeDelta();

			const FString Envelope = FUnrealOpenMcpBridgeEnvelope::BuildSuccessWithGate(TEXT("{\"k\":1}"), G);
			TestTrue(TEXT("ok true"), Envelope.Contains(TEXT("\"ok\":true")));
			TestTrue(TEXT("result spliced"), Envelope.Contains(TEXT("\"result\":{\"k\":1}")));
			TestTrue(TEXT("gate block"), Envelope.Contains(TEXT("\"gate\":{")));
			TestTrue(TEXT("ran true"), Envelope.Contains(TEXT("\"ran\":true")));
			TestTrue(TEXT("outcome passed"), Envelope.Contains(TEXT("\"outcome\":\"passed\"")));
			TestTrue(TEXT("checkpointId"), Envelope.Contains(TEXT("\"checkpointId\":\"cp_abcdef\"")));
		});

		It("BuildError keeps the P2.1 shape (no gate block when called without one)", [this]()
		{
			const FString Envelope = FUnrealOpenMcpBridgeEnvelope::BuildError(TEXT("timeout"), TEXT("ran out"));
			TestTrue(TEXT("ok false"), Envelope.Contains(TEXT("\"ok\":false")));
			TestTrue(TEXT("error code"), Envelope.Contains(TEXT("\"code\":\"timeout\"")));
			TestFalse(TEXT("no gate block"), Envelope.Contains(TEXT("\"gate\"")));
		});

		It("BuildPathsHintRequired names the tool + effective mode", [this]()
		{
			const FString Body = FUnrealOpenMcpBridgeEnvelope::BuildPathsHintRequired(
				TEXT("unreal_open_mcp_actor_create"), EUnrealOpenMcpGateMode::Enforce);
			TestTrue(TEXT("ok false"), Body.Contains(TEXT("\"ok\":false")));
			TestTrue(TEXT("paths_hint_required code"), Body.Contains(TEXT("\"code\":\"paths_hint_required\"")));
			TestTrue(TEXT("names the tool"), Body.Contains(TEXT("unreal_open_mcp_actor_create")));
			TestTrue(TEXT("effective mode"), Body.Contains(TEXT("\"effectiveMode\":\"enforce\"")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
