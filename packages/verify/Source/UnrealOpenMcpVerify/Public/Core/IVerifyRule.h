// IVerifyRule — the contract every verify rule implements.
//
// Ported from Unity Open MCP packages/verify/Editor/Core/IVerifyRule.cs at
// copy fidelity. A rule scans the supplied scope under the given run mode
// and appends zero or more FVerifyIssue into the sink. Concrete rule
// scanners land in P3.2–P3.4 / P3.7; P3.1 ships only the contract.
//
// Every rule must declare a stable Id (surfaced in MCP tool responses, the
// capability catalog, and the gate delta) — packages/verify/AGENTS.md §Verify
// rules. Scan() must be side-effect-free beyond appending to the sink: the
// runner swallows exceptions thrown from Scan() so one bad rule cannot abort
// a gate pass.
//
// Plain abstract C++ class (not a UObject) — verify contracts are value /
// interface types, not reflected objects. The gate (P3.5) holds rules by
// TUniquePtr<IVerifyRule>.
#pragma once

#include "CoreMinimal.h"

#include "Core/VerifyIssue.h"
#include "Core/VerifyRunMode.h"
#include "Core/VerifyScope.h"

/**
 * Contract for a verify rule. Implementations live under Rules/{RuleName}/.
 */
class UNREALOPENMCPVERIFY_API IVerifyRule
{
public:
	virtual ~IVerifyRule() = default;

	/** Stable rule Id (e.g. future "broken_soft_references"). */
	virtual FString GetId() const = 0;

	/**
	 * Scan the supplied scope under the given run mode, appending zero or
	 * more FVerifyIssue into the sink. Implementations must be side-effect-
	 * free beyond appending to the sink.
	 */
	virtual void Scan(const FVerifyScope& Scope, const EVerifyRunMode Mode, TArray<FVerifyIssue>& Sink) const = 0;
};
