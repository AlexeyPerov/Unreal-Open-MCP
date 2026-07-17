// FFixProviderRegistry implementation. See FixProviderRegistry.h for the
// Safe-flag rationale and the synthetic-key strategy.
#include "Fixes/FixProviderRegistry.h"

namespace UnrealOpenMcpVerify
{

// Process-wide registry. Owning pointers. Game-thread only — the registry is
// driven from the gate flow on the game thread. Function-local static so
// initialization order is well-defined across translation units.
TArray<TUniquePtr<IFixProvider>>& RegisteredProviders()
{
	static TArray<TUniquePtr<IFixProvider>> Providers;
	return Providers;
}

bool HasProviderWithId(const FString& FixId)
{
	const auto& Providers = RegisteredProviders();
	for (const TUniquePtr<IFixProvider>& Provider : Providers)
	{
		if (Provider.IsValid() && Provider->GetFixId() == FixId)
		{
			return true;
		}
	}
	return false;
}

// Resolve a provider's Safe flag via Describe(). Defaults to unsafe when
// Describe throws so the gate never auto-applies something it cannot reason
// about. UE has no equivalent of C# exceptions for non-fatal errors in pure
// contract code, but we keep the try/catch seam so a future provider that
// does throw (e.g. a plugin-supplied provider that hits a check()) is
// handled gracefully — matches Unity parity and the regression guard.
//
// Guarded by WITH_EXCEPTIONS so the build stays green in configurations where
// C++ exceptions are disabled (the production packaging default). In those
// builds a throwing Describe() surfaces as a crash the operator can triage
// rather than being silently swallowed; contract providers are expected to
// follow UE conventions (ensure()/check()) rather than throw.
bool TryResolveSafe(const IFixProvider& Provider, const FString& IssueId)
{
#if WITH_EXCEPTIONS
	try
	{
		return Provider.Describe(IssueId).bSafe;
	}
	catch (...)
	{
		return false;
	}
#else
	return Provider.Describe(IssueId).bSafe;
#endif
}

} // namespace UnrealOpenMcpVerify

void FFixProviderRegistry::RegisterDefaults()
{
	// P3.1 placeholder — the concrete fix providers (remove_missing_script,
	// relink_broken_guid, remove_orphan_meta, fix_duplicate_guid,
	// reassign_missing_texture, reassign_missing_shader) land in P3.7.
}

void FFixProviderRegistry::EnsureDefaultsRegistered()
{
	using namespace UnrealOpenMcpVerify;
	if (RegisteredProviders().Num() == 0)
	{
		RegisterDefaults();
	}
}

void FFixProviderRegistry::RegisterProvider(TUniquePtr<IFixProvider> Provider)
{
	using namespace UnrealOpenMcpVerify;
	if (!Provider.IsValid())
	{
		return;
	}
	if (HasProviderWithId(Provider->GetFixId()))
	{
		return;
	}
	RegisteredProviders().Add(MoveTemp(Provider));
}

IFixProvider* FFixProviderRegistry::Find(const FString& FixId)
{
	using namespace UnrealOpenMcpVerify;
	auto& Providers = RegisteredProviders();
	for (TUniquePtr<IFixProvider>& Provider : Providers)
	{
		if (Provider.IsValid() && Provider->GetFixId() == FixId)
		{
			return Provider.Get();
		}
	}
	return nullptr;
}

bool FFixProviderRegistry::TryGetFixInfo(
	const FString& RuleId,
	const FString& IssueCode,
	FString& OutFixId,
	bool& OutSafe)
{
	using namespace UnrealOpenMcpVerify;

	OutFixId.Reset();
	OutSafe = false;

	if (RuleId.IsEmpty() || IssueCode.IsEmpty())
	{
		return false;
	}

	const FString TestKey = SyntheticKey(RuleId, IssueCode);
	auto& Providers = RegisteredProviders();
	for (const TUniquePtr<IFixProvider>& Provider : Providers)
	{
		if (!Provider.IsValid() || !Provider->CanFix(TestKey))
		{
			continue;
		}
		OutFixId = Provider->GetFixId();
		OutSafe = TryResolveSafe(*Provider, TestKey);
		return true;
	}
	return false;
}

TArray<FString> FFixProviderRegistry::FixesForIssue(const FString& IssueId)
{
	using namespace UnrealOpenMcpVerify;

	TArray<FString> Result;
	if (IssueId.IsEmpty())
	{
		return Result;
	}
	auto& Providers = RegisteredProviders();
	for (const TUniquePtr<IFixProvider>& Provider : Providers)
	{
		if (Provider.IsValid() && Provider->CanFix(IssueId))
		{
			Result.Add(Provider->GetFixId());
		}
	}
	return Result;
}

TArray<FFixCandidate> FFixProviderRegistry::CandidatesForIssue(
	const FString& RuleId,
	const FString& IssueCode)
{
	using namespace UnrealOpenMcpVerify;

	TArray<FFixCandidate> Result;
	if (RuleId.IsEmpty() || IssueCode.IsEmpty())
	{
		return Result;
	}
	const FString TestKey = SyntheticKey(RuleId, IssueCode);
	auto& Providers = RegisteredProviders();
	for (const TUniquePtr<IFixProvider>& Provider : Providers)
	{
		if (!Provider.IsValid() || !Provider->CanFix(TestKey))
		{
			continue;
		}
		FFixCandidate Candidate;
		Candidate.FixId = Provider->GetFixId();
		Candidate.bSafe = TryResolveSafe(*Provider, TestKey);
		Result.Add(MoveTemp(Candidate));
	}
	return Result;
}

TArray<FString> FFixProviderRegistry::AvailableFixIds()
{
	using namespace UnrealOpenMcpVerify;

	TArray<FString> Result;
	auto& Providers = RegisteredProviders();
	for (const TUniquePtr<IFixProvider>& Provider : Providers)
	{
		if (Provider.IsValid())
		{
			Result.Add(Provider->GetFixId());
		}
	}
	return Result;
}

void FFixProviderRegistry::Clear()
{
	UnrealOpenMcpVerify::RegisteredProviders().Empty();
}
