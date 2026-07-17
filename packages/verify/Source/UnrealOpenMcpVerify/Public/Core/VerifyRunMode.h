// Run mode passed to IVerifyRule::Scan. Mirrors Unity's VerifyRunMode
// (packages/verify/Editor/Core/VerifyRunMode.cs) so later rules can skip
// heavy passes during the lightweight Checkpoint phase.
//
//   Checkpoint — the pre-mutation snapshot. Must be cheap (the runner budgets
//                it; see FVerifyRunner::CheckpointBudgetMs). Rules that walk
//                the Asset Registry or compile assets may defer expensive
//                work to Validate/Full.
//   Validate    — the post-mutation scope check (paths_hint-bounded).
//   Full        — every rule runs unconditionally (scan_all / scan_paths with
//                no mode hint).
#pragma once

#include "CoreMinimal.h"

#include "VerifyRunMode.generated.h"

UENUM(BlueprintType)
enum class EVerifyRunMode : uint8
{
	Checkpoint = 0,
	Validate = 1,
	Full = 2,
};
