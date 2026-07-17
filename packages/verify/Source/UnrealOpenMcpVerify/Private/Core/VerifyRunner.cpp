// FVerifyRunner implementation. See VerifyRunner.h for the lifecycle.
#include "Core/VerifyRunner.h"

#include "Core/IssueKey.h"
#include "Rules/BrokenSoftReferences/BrokenSoftReferencesRule.h"

#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"

#if WITH_EXCEPTIONS
#include <exception> // std::exception in the rule-scan try/catch seam
#endif

namespace UnrealOpenMcpVerify
{

// Process-wide registry. Owning pointers — rules are registered once and
// queried by reference. Access is game-thread only (the runner is driven
// from the editor's gate flow on the game thread); the static surface keeps
// parity with Unity's static VerifyRunner.
//
// Defined as a function-local static so initialization order is well-defined
// across translation units (the Unity version relies on a static field
// initialized at class load).
TArray<TUniquePtr<IVerifyRule>>& RegisteredRules()
{
	static TArray<TUniquePtr<IVerifyRule>> Rules;
	return Rules;
}

bool HasRuleWithId(const FString& Id)
{
	const auto& Rules = RegisteredRules();
	for (const TUniquePtr<IVerifyRule>& Rule : Rules)
	{
		if (Rule.IsValid() && Rule->GetId() == Id)
		{
			return true;
		}
	}
	return false;
}

// Log a swallowed rule-scan exception. The verify module does not yet own a
// dedicated log category (P3.1 keeps the module standalone — no shared log
// header from the bridge), so route through LogTemp with a recognizable
// prefix so a human watching Output Log can grep it. A later task may add a
// LogUnrealOpenMcpVerify category; until then LogTemp keeps the seam visible
// without coupling verify to the bridge's log header.
void LogRuleThrew(const FString& RuleId, const char* Message)
{
	UE_LOG(
		LogTemp,
		Warning,
		TEXT("[UnrealOpenMcpVerify] Rule '%s' threw during Scan: %s"),
		*RuleId,
		UTF8_TO_TCHAR(Message));
}

void LogCheckpointOverBudget(const int64 ElapsedMs, const FVerifyScope& Scope)
{
	const FString PathsJoined = FString::Join(Scope.Paths, TEXT(", "));
	UE_LOG(
		LogTemp,
		Warning,
		TEXT("[UnrealOpenMcpVerify] Checkpoint took %lldms (budget: %lldms) for paths: %s"),
		ElapsedMs,
		FVerifyRunner::CheckpointBudgetMs,
		*PathsJoined);
}

} // namespace UnrealOpenMcpVerify

void FVerifyRunner::RegisterDefaults()
{
	// P3.2: the first concrete rule family lands here. The remaining families
	// (missing_blueprint_parent, compile_error, content_path_hygiene) arrive
	// in P3.3–P3.4 / P3.7. Each rule is registered exactly once via
	// RegisterRule, which short-circuits on a duplicate Id so a hot reload
	// cannot double-register.
	RegisterRule(MakeUnique<FBrokenSoftReferencesRule>());
}

void FVerifyRunner::EnsureDefaultsRegistered()
{
	using namespace UnrealOpenMcpVerify;
	if (RegisteredRules().Num() == 0)
	{
		RegisterDefaults();
	}
}

const TArray<TUniquePtr<IVerifyRule>>& FVerifyRunner::GetRules()
{
	return UnrealOpenMcpVerify::RegisteredRules();
}

void FVerifyRunner::RegisterRule(TUniquePtr<IVerifyRule> Rule)
{
	using namespace UnrealOpenMcpVerify;
	if (!Rule.IsValid())
	{
		return;
	}
	if (HasRuleWithId(Rule->GetId()))
	{
		return;
	}
	RegisteredRules().Add(MoveTemp(Rule));
}

void FVerifyRunner::ClearRules()
{
	UnrealOpenMcpVerify::RegisteredRules().Empty();
}

FVerifyResult FVerifyRunner::RunScoped(
	const FVerifyScope& Scope,
	const TArray<FString>& RuleIds,
	const EVerifyRunMode Mode)
{
	using namespace UnrealOpenMcpVerify;

	const double StartSeconds = FPlatformTime::Seconds();

	TArray<FVerifyIssue> Issues;
	TArray<FString> CategoriesRun;
	TArray<FString> UnknownRuleIds;
	TArray<FString> AvailableRuleIds;

	auto& Rules = RegisteredRules();

	// Build the available roster (every registered Id) up front so a caller
	// asking for an unknown rule gets the "did you mean …?" list regardless
	// of which branch below executes.
	for (const TUniquePtr<IVerifyRule>& Rule : Rules)
	{
		if (Rule.IsValid())
		{
			AvailableRuleIds.Add(Rule->GetId());
		}
	}

	// Decide which rules to actually invoke. When RuleIds is empty we run
	// every registered rule; otherwise we run the intersection and report
	// any requested Id that is not registered as Unknown.
	bool bFilter = RuleIds.Num() > 0;
	if (bFilter)
	{
		// Unknown = requested − registered.
		for (const FString& Requested : RuleIds)
		{
			if (!HasRuleWithId(Requested))
			{
				UnknownRuleIds.Add(Requested);
			}
		}
	}

	for (const TUniquePtr<IVerifyRule>& Rule : Rules)
	{
		if (!Rule.IsValid())
		{
			continue;
		}
		if (bFilter && !RuleIds.Contains(Rule->GetId()))
		{
			continue;
		}

		CategoriesRun.Add(Rule->GetId());
#if WITH_EXCEPTIONS
		try
		{
			Rule->Scan(Scope, Mode, Issues);
		}
		catch (const std::exception& E)
		{
			LogRuleThrew(Rule->GetId(), E.what());
		}
		catch (...)
		{
			LogRuleThrew(Rule->GetId(), "unknown exception");
		}
#else
		// Exceptions disabled in this build configuration — rule Scan() bodies
		// are expected to follow UE conventions (ensure()/check()) rather than
		// throw, so a misbehaving rule surfaces as a crash the operator can
		// triage rather than a silent swallow. The WITH_EXCEPTIONS branch above
		// preserves the Unity parity behavior (scan → swallow → log) in editor
		// builds where exceptions are enabled.
		Rule->Scan(Scope, Mode, Issues);
#endif
	}

	const int64 DurationMs = static_cast<int64>((FPlatformTime::Seconds() - StartSeconds) * 1000.0);

	if (Mode == EVerifyRunMode::Checkpoint && DurationMs > CheckpointBudgetMs)
	{
		LogCheckpointOverBudget(DurationMs, Scope);
	}

	return FVerifyResult(
		MoveTemp(Issues),
		MoveTemp(CategoriesRun),
		DurationMs,
		MoveTemp(UnknownRuleIds),
		MoveTemp(AvailableRuleIds));
}

FCheckpointFingerprint FVerifyRunner::CreateCheckpoint(const FVerifyScope& Scope, const TArray<FString>& RuleIds)
{
	using namespace UnrealOpenMcpVerify;

	const FVerifyResult Result = RunScoped(Scope, RuleIds, EVerifyRunMode::Checkpoint);

	// cp_xxxxxx — 6 hex chars of a fresh GUID. Matches Unity's truncation
	// (Guid.NewGuid().ToString("N").Substring(0, 6)).
	const FGuid Guid = FGuid::NewGuid();
	const FString Id = TEXT("cp_") + Guid.ToString(EGuidFormats::Digits).Left(6);

	TMap<FString, FRuleFingerprint> Fingerprints;
	for (const FString& Category : Result.CategoriesRun)
	{
		int32 Errors = 0;
		int32 Warnings = 0;
		TSet<FString> Keys;
		for (const FVerifyIssue& Issue : Result.Issues)
		{
			if (Issue.RuleId != Category)
			{
				continue;
			}
			if (Issue.Severity == EVerifySeverity::Error)
			{
				++Errors;
			}
			else
			{
				++Warnings;
			}
			Keys.Add(FIssueKey::Build(Issue));
		}
		Fingerprints.Add(Category, FRuleFingerprint(Errors, Warnings, MoveTemp(Keys)));
	}

	return FCheckpointFingerprint(MoveTemp(Id), MoveTemp(Fingerprints));
}
