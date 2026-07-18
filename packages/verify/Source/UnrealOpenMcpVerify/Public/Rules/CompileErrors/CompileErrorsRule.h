// FCompileErrorsRule — greenfield verify rule that surfaces active C++ /
// Live Coding compile failures.
//
// No Unity Open MCP analogue: Unity's compile wait is bridge-transport
// behavior, not a verify rule with a stable issue code. See the rule
// constants header for the greenfield rationale.
//
// The rule owns its own ICompileStatusProvider so:
//   - RegisterDefaults() wires the production Live Coding / message-log
//     reader
//   - tests inject a fake provider to exercise the rule against synthetic
//     diagnostics without requiring a real broken build in CI
//
// Detection strategy (one-shot, read-only):
//   - Ask the provider for the coarse ECompileState.
//   - When Clean OR InProgressOrUnknown: emit nothing (a clean project has
//     no findings; a compile in flight has no stable result yet).
//   - When Failed: ask the provider for structured diagnostics. Emit one
//     finding per diagnostic. When the diagnostics list is empty, emit a
//     single coarse "(project)" finding so a known failure is never
//     silently swallowed.
//
// Per packages/verify/AGENTS.md §Verify rules:
//   - GetId() is the stable "compile_errors"
//   - every FVerifyIssue carries IssueCode "compile_error"
//   - Scan() appends only to the sink (the runner swallows exceptions)
//
// paths_hint handling (specs/execution/P3/P3.4.md):
//   - Compile failures are project-wide state, not path-local. The rule
//     therefore ALWAYS consults the provider regardless of scope.
//   - When a non-empty paths_hint is supplied, the rule filters its
//     per-file diagnostics by File-prefix match against the hint set so a
//     scoped Validate pass stays bounded; the coarse "(project)" finding
//     is still emitted when no per-file diagnostics match, because a known
//     failure must remain visible even outside the touched paths.
//   - An empty paths_hint (whole-project Full scan) reports every finding
//     unfiltered.
#pragma once

#include "CoreMinimal.h"

#include "Core/IVerifyRule.h"
#include "Rules/CompileErrors/ICompileStatusProvider.h"

#include "Templates/UniquePtr.h"

/**
 * Surfaces active C++ / Live Coding compile failures as compile_error
 * Errors.
 */
class UNREALOPENMCPVERIFY_API FCompileErrorsRule : public IVerifyRule
{
public:
	/**
	 * Construct with a custom provider (tests). The rule holds the provider
	 * by ownership so a fake can outlive the rule's Scan() calls.
	 */
	explicit FCompileErrorsRule(TUniquePtr<ICompileStatusProvider> InProvider);

	/** Default constructor wires the production Live Coding reader. */
	FCompileErrorsRule();

	virtual FString GetId() const override;
	virtual void Scan(const FVerifyScope& Scope, const EVerifyRunMode Mode, TArray<FVerifyIssue>& Sink) const override;

private:
	TUniquePtr<ICompileStatusProvider> Provider;
};
