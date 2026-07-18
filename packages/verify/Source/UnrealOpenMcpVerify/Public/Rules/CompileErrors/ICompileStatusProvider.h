// ICompileStatusProvider — the seam the compile_errors scanner uses to ask
// "is the project's C++ currently in a failed-compile state, and what are
// the active diagnostics?".
//
// Mirrors the IBlueprintParentResolver pattern (P3.3) and ISoftPathResolver
// (P3.2): the production provider queries read-only Live Coding / module
// status APIs; tests inject a fake so the rule can be exercised against
// synthetic diagnostics without authoring a real broken build (which a CI
// runner could not recover from cleanly).
//
// CRITICAL contract: implementations MUST be read-only. The hard rule from
// specs/execution/P3/P3.4.md is "do not kick off a new compile as a side
// effect" — the verify rule runs inside the gate flow on the game thread,
// and a Compile() call would block for 20-60s, surface a modal, or wedge
// the editor. Implementations therefore read already-known state only:
//   - Live Coding: IsEnabledForSession / HasStarted / IsCompiling status
//     (NOT Compile — that is the side effect we ban)
//   - Module manager: whether any module is currently mid-recompile
//   - Cached compile-result notifications the editor already received
//
// Per packages/verify/AGENTS.md §Verify rules: the production provider
// must never throw — the runner swallows exceptions, but a provider that
// throws will still abort the current rule's remaining work in
// no-exception builds.
//
// Plain abstract C++ class (not a UObject) — same convention as the other
// rule seams (IBlueprintParentResolver, ISoftPathResolver). The associated
// FCompileDiagnostic / ECompileState types live at global scope alongside
// the interface, mirroring EBlueprintParentResolution next to
// IBlueprintParentResolver.
#pragma once

#include "CoreMinimal.h"

// One structured compiler diagnostic. Mirrors the Unreal-MCP
// FSourceDiagnostic shape (file/line/severity/message) — studied for
// structuring, not copied (Unreal-MCP parses UBT stdout; this rule reads
// live status, so the provider surfaces the same shape from whatever
// source it consults).
struct FCompileDiagnostic
{
	// Source file path the diagnostic pinned. May be empty for a coarse
	// "Live Coding failed" finding that carries no per-file breakdown (the
	// rule then falls back to the "(project)" AssetPath sentinel).
	FString File;

	// 1-based line number, or 0 when the provider has no line.
	int32 Line = 0;

	// 1-based column number, or 0 when the provider has no column.
	int32 Column = 0;

	// Human-readable diagnostic message (e.g. "'Foo': undeclared identifier").
	FString Message;

	// Owning module name when known (e.g. "UnrealOpenMcpVerify"), empty
	// otherwise. The rule surfaces it as evidence so an operator can jump
	// to the right Build.cs.
	FString Module;
};

// Coarse overall state of the project's C++ compile. The rule emits at
// least one finding when the state is Failed; emits nothing when Clean;
// and emits nothing when InProgressOrUnknown (a compile in flight has no
// stable result yet, and an unknown state must not produce noise).
enum class ECompileState : uint8
{
	// No known compile failures. The rule emits nothing.
	Clean = 0,
	// A compile failure is currently known. The rule emits one finding per
	// diagnostic returned by GetDiagnostics, plus a coarse "(project)"
	// finding when the diagnostics list is empty (a known failure with no
	// per-file breakdown).
	Failed = 1,
	// A compile is currently in flight, or the provider has no signal at
	// all. The rule emits nothing in both cases: an in-flight compile has
	// no stable result yet (a finding would be stale before it reached the
	// agent), and an unknown state must not produce noise.
	InProgressOrUnknown = 2,
};

/**
 * Read-only compile-status seam. See file header for the hard "no compile
 * side effects" contract.
 */
class UNREALOPENMCPVERIFY_API ICompileStatusProvider
{
public:
	virtual ~ICompileStatusProvider() = default;

	/**
	 * @return the current coarse compile state. Implementations must never
	 *         trigger a compile to compute this — read already-known state
	 *         only (Live Coding status flags, cached notifications, the
	 *         module manager's recompile-pending set).
	 */
	virtual ECompileState GetState() const = 0;

	/**
	 * @param OutDiagnostics filled with the current structured compiler
	 *                       diagnostics when GetState() == Failed. May be
	 *                       left empty even on Failed (a coarse Live Coding
	 *                       failure carries no per-file breakdown) — the
	 *                       rule synthesizes a single "(project)" finding
	 *                       in that case.
	 *
	 * Implementations must never trigger a compile to populate this list.
	 * Callers should only invoke this when GetState() == Failed; calling
	 * it in other states is allowed but the provider may return empty.
	 */
	virtual void GetDiagnostics(TArray<FCompileDiagnostic>& OutDiagnostics) const = 0;
};
