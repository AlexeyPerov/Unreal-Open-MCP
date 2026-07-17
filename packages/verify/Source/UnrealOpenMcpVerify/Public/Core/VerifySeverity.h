// Severity for a single VerifyIssue.
//
// Ported from Unity Open MCP packages/verify/Editor/Core/VerifyIssue.cs
// (VerifySeverity enum). Severity is set per-issue, not per-rule: a single
// rule can emit both Errors and Warnings. The gate delta treats Errors as
// failures; Warnings are informational (packages/verify/AGENTS.md §Verify
// rules).
#pragma once

#include "CoreMinimal.h"

#include "VerifySeverity.generated.h"

UENUM(BlueprintType)
enum class EVerifySeverity : uint8
{
	Error = 0,
	Warning = 1,
};
