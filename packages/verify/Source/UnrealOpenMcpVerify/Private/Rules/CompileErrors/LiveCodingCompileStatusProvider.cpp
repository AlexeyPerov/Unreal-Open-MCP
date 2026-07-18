// FLiveCodingCompileStatusProvider implementation. See header for the
// read-only detection strategy and the "no compile side effects" hard ban.
//
// Per packages/verify/AGENTS.md §Verify rules: the provider must never
// throw — the runner swallows exceptions, but a provider that throws will
// still abort the current rule's remaining work in no-exception builds.
// All engine-API consults here are null-checked and degrade to the
// InProgressOrUnknown / Clean defaults when an API is unavailable.
#include "Rules/CompileErrors/LiveCodingCompileStatusProvider.h"

// ILiveCodingModule status flags are read-only: IsEnabledForSession(),
// HasStarted(), IsCompiling(). We never call Compile() — that is the side
// effect this rule bans. The LiveCoding module is Windows-only in stock
// UE (Engine/Source/Developer/Windows/LiveCoding), so gate the include +
// the consult behind the platform + the module being loadable at runtime.
#if PLATFORM_WINDOWS
#include "ILiveCodingModule.h"
#include "Modules/ModuleManager.h"
#endif // PLATFORM_WINDOWS

namespace UnrealOpenMcpVerify::CompileErrors
{

namespace
{

#if PLATFORM_WINDOWS
// True when Live Coding is loaded and active for this editor session.
// Read-only; never triggers a compile.
bool IsLiveCodingActive()
{
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(FName(LIVE_CODING_MODULE_NAME));
	return LiveCoding != nullptr
		&& LiveCoding->IsEnabledForSession()
		&& LiveCoding->HasStarted();
}

// True when a Live Coding compile is currently in flight. Read-only.
bool IsLiveCodingCompiling()
{
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(FName(LIVE_CODING_MODULE_NAME));
	return LiveCoding != nullptr && LiveCoding->IsCompiling();
}
#endif // PLATFORM_WINDOWS

} // namespace

ECompileState FLiveCodingCompileStatusProvider::GetState() const
{
#if PLATFORM_WINDOWS
	// A compile in flight has no stable result yet. A finding emitted now
	// would be stale before it reached the agent — surface
	// InProgressOrUnknown so the rule emits nothing and the next gate pass
	// re-reads after the compile settles.
	if (IsLiveCodingCompiling())
	{
		return ECompileState::InProgressOrUnknown;
	}

	// Live Coding is loaded but not compiling. v1 of this rule is honest
	// about the limits of the read-only public API: ILiveCodingModule does
	// not expose the last compile result through a public method, and a
	// structured Message Log scrape for compile diagnostics is brittle
	// across UE versions (log names and per-message shape drift).
	//
	// Rather than emit a misleading Clean (would mask real failures the
	// rule cannot see) OR a misleading Failed (would emit noise on every
	// clean project), v1 returns InProgressOrUnknown. The rule therefore
	// produces no findings in a stock production editor until either:
	//   - a future task wires a reliable structured-diagnostic source
	//     (engine-internal compile-result broadcast), or
	//   - the Phase 8 read_compile_errors offline Editor.log parser
	//     surfaces failures out-of-band.
	//
	// The rule's behavior contract is still fully pinned by the Automation
	// spec, which injects a fake provider to exercise the Failed / Clean /
	// InProgressOrUnknown branches deterministically. Production consumers
	// get a no-op rule today; that is the documented v1 fidelity
	// (specs/execution/P3/P3.4.md "greenfield" + the v1 scope carve-out
	// for offline Editor.log reads in Phase 8).
	if (IsLiveCodingActive())
	{
		return ECompileState::InProgressOrUnknown;
	}
#endif // PLATFORM_WINDOWS

	// No Live Coding signal available (non-Windows, headless session, or
	// the module is not loaded). An editor without Live Coding has no
	// hot-reload failure state this rule can observe — surface Clean so
	// the rule stays silent. The bridge's source-compile tool surfaces its
	// own UBT report separately (P7).
	return ECompileState::Clean;
}

void FLiveCodingCompileStatusProvider::GetDiagnostics(TArray<FCompileDiagnostic>& OutDiagnostics) const
{
	// v1 production provider reports the coarse state via GetState() only.
	// Per the header: a structured Message Log scrape for per-file
	// diagnostics is brittle across UE versions, so v1 leaves this list
	// empty. When GetState() returns Failed, the rule synthesizes a single
	// "(project)" finding with the coarse Live Coding failure — never
	// silently swallows a known failure.
	//
	// The fake provider in the Automation spec exercises the per-diagnostic
	// path so the rule's structured-evidence contract is pinned regardless
	// of what the production provider emits today.
	OutDiagnostics.Reset();
}

} // namespace UnrealOpenMcpVerify::CompileErrors
