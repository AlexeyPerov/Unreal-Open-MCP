// FUnrealOpenMcpGatePolicy implementation. See header for the outcome taxonomy
// and the checkpoint → mutate → validate → delta contract.
//
// Ported from Unity Open MCP packages/bridge/Editor/Gate/GatePolicy.cs at copy
// fidelity. The validate scan is invoked via FUnrealOpenMcpVerifyGateAdapter
// (the bridge → verify glue). A validate-scan throw is caught here and
// surfaced as ValidateScanFailed — the mutation already committed; the agent
// should run validate_edit / scan_paths manually to confirm health. WITH_EXCEPTIONS
// guards the catch (Unreal editor builds may or may not enable exceptions; the
// runner itself logs and skips faulting rules).
#include "Gate/UnrealOpenMcpGatePolicy.h"

#include "Gate/UnrealOpenMcpCheckpointStore.h"
#include "Gate/UnrealOpenMcpVerifyGateAdapter.h"
#include "UnrealOpenMcpLog.h"

#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"

#if WITH_EXCEPTIONS
#include <exception> // std::exception in the validate-scan catch
#endif

void FUnrealOpenMcpGatePolicy::ResolveOutcome(
	EUnrealOpenMcpGateMode Mode,
	const FUnrealOpenMcpGateDelta& Delta,
	EUnrealOpenMcpGateOutcome& OutOutcome,
	bool& bOutGateFailed)
{
	// Decision matrix (Unity parity):
	//   new Errors → Enforce hard-fails; Warn soft-warns.
	//   new Warnings only → Warn soft-warns; Enforce/Off pass.
	//   clean → Passed.
	//
	// ValidateScanFailed is NEVER returned here — it is set only by the
	// validate-exception catch in Execute. Returning it here would leak the
	// scanner-failure outcome into normal delta resolution and break the
	// parity invariant (pinned in GatePolicySpec).
	if (Delta.NewErrors > 0)
	{
		if (Mode == EUnrealOpenMcpGateMode::Enforce)
		{
			OutOutcome = EUnrealOpenMcpGateOutcome::Failed;
			bOutGateFailed = true;
		}
		else
		{
			OutOutcome = EUnrealOpenMcpGateOutcome::Warned;
			bOutGateFailed = false;
		}
		return;
	}

	if (Delta.NewWarnings > 0)
	{
		if (Mode == EUnrealOpenMcpGateMode::Warn)
		{
			OutOutcome = EUnrealOpenMcpGateOutcome::Warned;
			bOutGateFailed = false;
		}
		else
		{
			OutOutcome = EUnrealOpenMcpGateOutcome::Passed;
			bOutGateFailed = false;
		}
		return;
	}

	OutOutcome = EUnrealOpenMcpGateOutcome::Passed;
	bOutGateFailed = false;
}

namespace UnrealOpenMcpGatePolicyInternal
{
	// Parsed issue key. Kept private to this TU — the only consumer is the
	// next-step builder; the canonical FIssueKey parser lives in the verify
	// module and is the contract every other caller should use.
	struct FIssueKeyParts
	{
		FString CategoryId;
		FString Severity;
		FString AssetPath;
		FString IssueCode;
	};

	// Parse the canonical "{RuleId}|{SEVERITY}|{AssetPath}|{IssueCode}" key
	// back into its parts. Returns false on malformed input (any part count
	// other than 4). Mirrors FIssueKey::TryParse and Unity's
	// GatePolicy.ParseIssueKey so the next-step builder can extract a hint
	// without depending on the verify module's parser.
	bool ParseIssueKey(const FString& Key, FIssueKeyParts& Out)
	{
		if (Key.IsEmpty())
		{
			return false;
		}
		TArray<FString> Parts;
		Key.ParseIntoArray(Parts, TEXT("|"), false);
		// Exactly 4 parts — fewer is malformed; more means a stray '|' leaked
		// into one of the fields and the key must be rejected (not silently
		// truncated). Mirrors FIssueKey::TryParse.
		if (Parts.Num() != 4)
		{
			return false;
		}
		Out.CategoryId = Parts[0];
		Out.Severity = Parts[1];
		Out.AssetPath = Parts[2];
		Out.IssueCode = Parts[3];
		return true;
	}

	// Map a known issue code (broken_soft_reference, missing_blueprint_parent,
	// compile_error) to a fix id the agent can apply via the apply_fix meta-
	// tool (lands in P3.7). The mapping stays loose — the gate must not crash
	// on an unknown code, and the apply_fix tool will reject an unknown fix id
	// itself. Mirrors Unity's TryFixIdForIssue but for the Unreal rule family.
	bool TryFixIdForIssue(const FString& CategoryId, const FString& IssueCode, FString& OutFixId)
	{
		// Issue codes carry an optional ":<suffix>" (e.g. "missing_guid:<guid>")
		// so a fix provider can identify the exact broken reference. Strip it
		// before matching against the bare code.
		FString Code = IssueCode;
		const int32 ColonIdx = Code.Find(TEXT(":"), ESearchCase::CaseSensitive);
		if (ColonIdx != INDEX_NONE)
		{
			Code = Code.Left(ColonIdx);
		}

		if (CategoryId == TEXT("broken_soft_references"))
		{
			OutFixId = TEXT("relink_broken_soft_reference");
			return true;
		}
		if (CategoryId == TEXT("missing_blueprint_parent"))
		{
			OutFixId = TEXT("reparent_blueprint");
			return true;
		}

		OutFixId.Reset();
		return false;
	}

	// Build the first-issue hint line that the next-step guidance embeds so an
	// agent reading the envelope gets the issue code + asset path inline.
	FString FormatIssue(const FIssueKeyParts& Parsed, const FString& RawKey)
	{
		return FString::Printf(TEXT("%s on %s"), *Parsed.IssueCode, *Parsed.AssetPath);
	}

	void AddIssueHints(
		TArray<FString>& OutSteps,
		const FUnrealOpenMcpGateDelta& Delta,
		const bool bIsFailed)
	{
		const bool bHasFirstKey = Delta.NewIssueKeys.Num() > 0;
		const FString FirstKey = bHasFirstKey ? Delta.NewIssueKeys[0] : FString();

		FIssueKeyParts Parsed;
		const bool bParsed = bHasFirstKey && ParseIssueKey(FirstKey, Parsed);

		const FString ModeLabel = bIsFailed ? FString() : FString(TEXT(" (warn mode)"));
		OutSteps.Add(FString::Printf(
			TEXT("Gate detected %d new error(s)%s. First: %s"),
			Delta.NewErrors,
			*ModeLabel,
			bParsed ? *FormatIssue(Parsed, FirstKey) : (bHasFirstKey ? *FirstKey : TEXT("unknown"))));

		if (bParsed)
		{
			FString FixId;
			if (TryFixIdForIssue(Parsed.CategoryId, Parsed.IssueCode, FixId))
			{
				OutSteps.Add(FString::Printf(
					TEXT("Consider unreal_open_mcp_apply_fix with fix_id %s (dry_run first)"),
					*FixId));
			}
			OutSteps.Add(FString::Printf(
				TEXT("Use unreal_open_mcp_find_references for %s to assess downstream impact"),
				*Parsed.AssetPath));
		}
		else
		{
			OutSteps.Add(TEXT("Review the affected asset and fix the introduced issue before retrying."));
		}

		if (bIsFailed)
		{
			OutSteps.Add(TEXT(
				"Fix the issue and retry; use unreal_open_mcp_validate_edit to verify without mutation."));
		}
	}
} // namespace UnrealOpenMcpGatePolicyInternal

TArray<FString> FUnrealOpenMcpGatePolicy::GenerateAgentNextSteps(
	const FUnrealOpenMcpGateDelta& Delta,
	EUnrealOpenMcpGateOutcome Outcome)
{
	namespace Internal = UnrealOpenMcpGatePolicyInternal;

	TArray<FString> Steps;
	switch (Outcome)
	{
		case EUnrealOpenMcpGateOutcome::Failed:
			Internal::AddIssueHints(Steps, Delta, /*bIsFailed=*/true);
			break;
		case EUnrealOpenMcpGateOutcome::Warned:
			if (Delta.NewErrors > 0)
			{
				Internal::AddIssueHints(Steps, Delta, /*bIsFailed=*/false);
			}
			else
			{
				Steps.Add(FString::Printf(
					TEXT("Gate detected %d new warning(s). Consider reviewing with unreal_open_mcp_validate_edit before proceeding."),
					Delta.NewWarnings));
			}
			break;
		case EUnrealOpenMcpGateOutcome::Passed:
			if (Delta.ResolvedErrors > 0)
			{
				Steps.Add(FString::Printf(
					TEXT("Gate passed — %d previously reported error(s) resolved."),
					Delta.ResolvedErrors));
			}
			else
			{
				Steps.Add(TEXT("Gate passed — no new issues detected."));
			}
			break;
		case EUnrealOpenMcpGateOutcome::Skipped:
		case EUnrealOpenMcpGateOutcome::ValidateScanFailed:
			// No structured next-steps guidance here — Skipped carries no
			// delta (gate did not run), and ValidateScanFailed's caller (the
			// Execute catch) supplies its own message.
			break;
	}
	return Steps;
}

FUnrealOpenMcpGateDispatchResult FUnrealOpenMcpGatePolicy::Execute(
	EUnrealOpenMcpGateMode Mode,
	const TArray<FString>& PathsHint,
	TFunctionRef<FUnrealOpenMcpToolDispatchResult()> Mutation)
{
	// Off mode → run the mutation ungated. Outcome follows the mutation result
	// (Skipped on success, Failed on mutation error).
	if (Mode == EUnrealOpenMcpGateMode::Off)
	{
		FUnrealOpenMcpToolDispatchResult MutationResult = Mutation();
		FUnrealOpenMcpGateDispatchResult Result;
		Result.Mutation = MoveTemp(MutationResult);
		Result.bGateRan = false;
		Result.Outcome = Result.Mutation.bOk ? EUnrealOpenMcpGateOutcome::Skipped : EUnrealOpenMcpGateOutcome::Failed;
		Result.bGateFailed = !Result.Mutation.bOk;
		return Result;
	}

	// Empty paths_hint → no whole-project fallback. Run the mutation ungated
	// and report Skipped (or Failed when the mutation itself fails). Same shape
	// as the Off branch; distinct outcome token so an agent can tell "user
	// asked for off" from "user omitted the hint".
	if (PathsHint.Num() == 0)
	{
		FUnrealOpenMcpToolDispatchResult MutationResult = Mutation();
		FUnrealOpenMcpGateDispatchResult Result;
		Result.Mutation = MoveTemp(MutationResult);
		Result.bGateRan = false;
		Result.Outcome = Result.Mutation.bOk ? EUnrealOpenMcpGateOutcome::Skipped : EUnrealOpenMcpGateOutcome::Failed;
		Result.bGateFailed = !Result.Mutation.bOk;
		return Result;
	}

	const double GateStartSeconds = FPlatformTime::Seconds();

	// 1) Checkpoint — the pre-mutation snapshot.
	FCheckpointFingerprint Checkpoint;
	int64 CheckpointMs = 0;
	{
		const double CpStart = FPlatformTime::Seconds();
		Checkpoint = FUnrealOpenMcpVerifyGateAdapter::CreateCheckpoint(PathsHint);
		CheckpointMs = static_cast<int64>((FPlatformTime::Seconds() - CpStart) * 1000.0);

		// Mirror the gate-run checkpoint into the in-memory store so the P3.6
		// delta / checkpoint_create meta-tools can delta-compare against it.
		// Best-effort — a storage failure must not break the gate path. The
		// store carries the gate-run's label-as-null so the meta-tool surfaces
		// it as "gate run" (not a user-tagged checkpoint).
#if WITH_EXCEPTIONS
		try
		{
			FUnrealOpenMcpCheckpointStoreEntry Entry;
			Entry.CheckpointId = Checkpoint.CheckpointId;
			Entry.TimestampUtc = FDateTime::UtcNow().ToIso8601();
			Entry.Label.Reset();
			Entry.Paths = PathsHint;
			Entry.Fingerprint = Checkpoint;
			FUnrealOpenMcpCheckpointStore::Store(MoveTemp(Entry));
		}
		catch (...)
		{
			// ignored — checkpoint history capture is non-essential.
		}
#else
		// No-exceptions build — Store is a plain TArray/TMap append that does
		// not throw; no guard needed.
		{
			FUnrealOpenMcpCheckpointStoreEntry Entry;
			Entry.CheckpointId = Checkpoint.CheckpointId;
			Entry.TimestampUtc = FDateTime::UtcNow().ToIso8601();
			Entry.Label.Reset();
			Entry.Paths = PathsHint;
			Entry.Fingerprint = Checkpoint;
			FUnrealOpenMcpCheckpointStore::Store(MoveTemp(Entry));
		}
#endif
	}

	// 2) Mutate. The mutation runs on the calling thread (the game thread).
	FUnrealOpenMcpToolDispatchResult MutationResult = Mutation();
	if (!MutationResult.bOk)
	{
		// Mutation failed before we could validate. Surface Failed and let the
		// agent fix the mutation error and retry — no delta is meaningful here.
		FUnrealOpenMcpGateDispatchResult Result;
		Result.Mutation = MoveTemp(MutationResult);
		Result.bGateRan = true;
		Result.Outcome = EUnrealOpenMcpGateOutcome::Failed;
		Result.CheckpointId = Checkpoint.CheckpointId;
		Result.CheckpointDurationMs = CheckpointMs;
		Result.TotalGateDurationMs = static_cast<int64>((FPlatformTime::Seconds() - GateStartSeconds) * 1000.0);
		Result.bGateFailed = true;
		Result.AgentNextSteps.Add(TEXT(
			"Mutation failed before gate could validate. Fix the mutation error and retry."));
		return Result;
	}

	// 3) Validate — the post-mutation scan. A throw here is distinct from a
	// delta failure: the mutation committed; the scanner blew up for an
	// unrelated reason. Surface ValidateScanFailed and recommend a manual
	// validate. Do NOT roll back — the mutation is good.
	FVerifyResult Validation;
#if WITH_EXCEPTIONS
	try
	{
		Validation = FUnrealOpenMcpVerifyGateAdapter::ValidatePaths(PathsHint);
	}
	catch (const std::exception& E)
	{
		FUnrealOpenMcpGateDispatchResult Result;
		Result.Mutation = MoveTemp(MutationResult);
		Result.bGateRan = true;
		Result.Outcome = EUnrealOpenMcpGateOutcome::ValidateScanFailed;
		Result.CheckpointId = Checkpoint.CheckpointId;
		Result.CheckpointDurationMs = CheckpointMs;
		Result.TotalGateDurationMs = static_cast<int64>((FPlatformTime::Seconds() - GateStartSeconds) * 1000.0);
		Result.bGateFailed = true;
		const FString Msg(UTF8_TO_TCHAR(E.what()));
		Result.AgentNextSteps.Add(FString::Printf(
			TEXT("Mutation committed, but the gate's validate scan threw (%s). "
				 "Run unreal_open_mcp_validate_edit (or unreal_open_mcp_scan_paths) on the "
				 "touched paths to confirm health."),
			*Msg));
		return Result;
	}
	catch (...)
	{
		FUnrealOpenMcpGateDispatchResult Result;
		Result.Mutation = MoveTemp(MutationResult);
		Result.bGateRan = true;
		Result.Outcome = EUnrealOpenMcpGateOutcome::ValidateScanFailed;
		Result.CheckpointId = Checkpoint.CheckpointId;
		Result.CheckpointDurationMs = CheckpointMs;
		Result.TotalGateDurationMs = static_cast<int64>((FPlatformTime::Seconds() - GateStartSeconds) * 1000.0);
		Result.bGateFailed = true;
		Result.AgentNextSteps.Add(TEXT(
			"Mutation committed, but the gate's validate scan threw (unknown exception). "
			"Run unreal_open_mcp_validate_edit (or unreal_open_mcp_scan_paths) on the "
			"touched paths to confirm health."));
		return Result;
	}
#else
	// No-exceptions build — Unreal editor builds may not enable EH; verify's
	// own runner logs + skips a faulting rule via its own WITH_EXCEPTIONS guard
	// (see VerifyRunner.cpp). A real crash here surfaces as a crash the
	// operator can triage rather than a silent swallow.
	Validation = FUnrealOpenMcpVerifyGateAdapter::ValidatePaths(PathsHint);
#endif

	// 4) Delta — checkpoint vs validate diff. Drives the outcome decision.
	FUnrealOpenMcpGateDelta Delta = FUnrealOpenMcpVerifyGateAdapter::ComputeDelta(Checkpoint, Validation);

	const int64 TotalMs = static_cast<int64>((FPlatformTime::Seconds() - GateStartSeconds) * 1000.0);
	if (TotalMs > GateBudgetMs)
	{
		const FString PathsJoined = FString::Join(PathsHint, TEXT(", "));
		UE_LOG(
			LogUnrealOpenMcp,
			Warning,
			TEXT("[Unreal Open MCP] GatePolicy: total gate path took %lldms (budget: %lldms, checkpoint: %lldms, validate: %lldms) for paths: %s"),
			TotalMs,
			GateBudgetMs,
			CheckpointMs,
			Validation.DurationMs,
			*PathsJoined);
	}

	EUnrealOpenMcpGateOutcome Outcome = EUnrealOpenMcpGateOutcome::Passed;
	bool bGateFailed = false;
	ResolveOutcome(Mode, Delta, Outcome, bGateFailed);

	FUnrealOpenMcpGateDispatchResult Result;
	Result.Mutation = MoveTemp(MutationResult);
	Result.bGateRan = true;
	Result.Outcome = Outcome;
	Result.CheckpointId = Checkpoint.CheckpointId;
	Result.CategoriesRun = Validation.CategoriesRun;
	Result.CheckpointDurationMs = CheckpointMs;
	Result.ValidationDurationMs = Validation.DurationMs;
	Result.TotalGateDurationMs = TotalMs;
	Result.Delta = MoveTemp(Delta);
	Result.bGateFailed = bGateFailed;
	Result.AgentNextSteps = GenerateAgentNextSteps(*Result.Delta, Outcome);
	return Result;
}

EUnrealOpenMcpGateMode FUnrealOpenMcpGatePolicy::ParseMode(const FString& Mode)
{
	if (Mode == TEXT("warn"))
	{
		return EUnrealOpenMcpGateMode::Warn;
	}
	if (Mode == TEXT("off"))
	{
		return EUnrealOpenMcpGateMode::Off;
	}
	return EUnrealOpenMcpGateMode::Enforce;
}

const TCHAR* FUnrealOpenMcpGatePolicy::OutcomeToken(EUnrealOpenMcpGateOutcome Outcome)
{
	switch (Outcome)
	{
		case EUnrealOpenMcpGateOutcome::Passed: return TEXT("passed");
		case EUnrealOpenMcpGateOutcome::Warned: return TEXT("warned");
		case EUnrealOpenMcpGateOutcome::Failed: return TEXT("failed");
		case EUnrealOpenMcpGateOutcome::Skipped: return TEXT("skipped");
		case EUnrealOpenMcpGateOutcome::ValidateScanFailed: return TEXT("validate_scan_failed");
	}
	return TEXT("unknown");
}
