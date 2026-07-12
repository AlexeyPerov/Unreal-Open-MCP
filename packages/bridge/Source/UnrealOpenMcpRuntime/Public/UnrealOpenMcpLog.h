// Dedicated log category for Unreal Open MCP. Declared in the Runtime module
// (the packaging home for shared types) so both Runtime and Editor code can
// log against the same category. Keeps Output Log messages actionable for
// agent/operator debugging.
#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

UNREALOPENMCPRUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogUnrealOpenMcp, Log, All);
