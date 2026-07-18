// FLiveCodingCompileStatusProvider — production ICompileStatusProvider.
//
// Greenfield. Unity Open MCP has no equivalent: Unity's compile wait is a
// bridge-transport behavior, not a verify rule with a status provider.
//
// v1 detection strategy — read-only, NEVER triggers a compile:
//   1. On Windows, ask ILiveCodingModule for its current status flags. If a
//      compile is in flight (IsCompiling() == true), return
//      InProgressOrUnknown: a compile in flight has no stable result yet,
//      and a finding emitted now would be stale before it reached the
//      agent. Mirrors the rule body's "no compile side effects" hard ban.
//   2. When Live Coding is loaded but not compiling, return
//      InProgressOrUnknown as well: ILiveCodingModule's public API does
//      not expose the last compile result through a read-only method, and
//      a structured Message Log scrape for compile diagnostics is brittle
//      across UE versions (log names and per-message shape drift). v1 is
//      honest about that limit rather than guessing. The rule's behavior
//      contract is still fully pinned by the Automation spec, which
//      injects a fake provider to exercise the Failed / Clean /
//      InProgressOrUnknown branches deterministically.
//   3. If Live Coding is unavailable (non-Windows platform, headless
//      session, or the module is not loaded), return Clean: an editor
//      without Live Coding has no hot-reload failure state this rule can
//      observe. The bridge's source-compile tool surfaces its own UBT
//      report separately (P7).
//
// Production consumers therefore get a no-op rule today. Two follow-ups
// unblock real findings:
//   - A future task wires a reliable structured-diagnostic source
//     (engine-internal compile-result broadcast).
//   - The Phase 8 read_compile_errors offline Editor.log parser surfaces
//     failures out-of-band.
//
// Per packages/verify/AGENTS.md §Verify rules: the provider must never
// throw — the runner swallows exceptions, but a provider that throws will
// still abort the current rule's remaining work in no-exception builds.
#pragma once

#include "CoreMinimal.h"

#include "Rules/CompileErrors/ICompileStatusProvider.h"

namespace UnrealOpenMcpVerify::CompileErrors
{

/**
 * Production ICompileStatusProvider. Read-only; never triggers a compile.
 * See the file header for the v1 detection strategy and engine-version
 * caveats.
 */
class FLiveCodingCompileStatusProvider final : public ICompileStatusProvider
{
public:
	virtual ECompileState GetState() const override;
	virtual void GetDiagnostics(TArray<FCompileDiagnostic>& OutDiagnostics) const override;
};

} // namespace UnrealOpenMcpVerify::CompileErrors
