// unreal_open_mcp_apply_fix — see header for the inner handler / gate runner
// split and the dry-run short-circuit contract.
//
// Ported from Unity Open MCP packages/bridge/Editor/MetaTools/ApplyFixTool.cs
// and ApplyFixGateRunner.cs at copy fidelity:
//   - The handler's arg parsing, dry-run preview, fix-listing, unknown-fix,
//     and apply paths are byte-for-byte ports of Unity's surface (the JSON
//     payload schema is a shared contract — agents that consume one consume
//     the other).
//   - The gate runner reuses FUnrealOpenMcpGatePolicy::Execute for the
//     checkpoint → apply → validate → delta sequence and adds a file-level
//     rollback step via FUnrealOpenMcpFixRollback.
//
// Unreal-specific deltas:
//   - Rollback scope = the issue's .uasset file path + companion .uexp/.ubulk
//     (predicted from the issue's asset path via FPackageName). Unity
//     snapshots Assets/<path> + .meta.
//   - The gate-runner-mediated rollback fires on (mutation failed) OR
//     (Enforce + delta.NewErrors > 0). Warn never rolls back (operator asked
//     for report-only); Off has no delta to check and surfaces a structured
//     rollbackDisabled warning instead.
#include "MetaTools/UnrealOpenMcpApplyFixTool.h"

#include "Bridge/UnrealOpenMcpBridgeJson.h"
#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Gate/UnrealOpenMcpFixRollback.h"
#include "Gate/UnrealOpenMcpGatePolicy.h"
#include "UnrealOpenMcpLog.h"

#include "Core/IssueKey.h"
#include "Fixes/FixContracts.h"
#include "Fixes/FixProviderRegistry.h"
#include "Core/VerifySeverity.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "Misc/PackageName.h"

namespace
{
	// Ambient flag set by FUnrealOpenMcpApplyFixGateRunner::Execute while a
	// FixRollback snapshot is active. The inner handler checks it to refuse a
	// non-dry-run apply dispatched WITHOUT the runner wrapper. Game-thread
	// only (bridge dispatch is serialized), so a plain static bool is safe.
	bool bRollbackSnapshotActive = false;

	// Parse the raw POST body into a JSON object. Empty body → empty object so
	// handlers can resolve optional fields with their defaults. Null when the
	// body is not a JSON object (caller surfaces a structured invalid_parameter
	// error). Mirrors the helper in the gate-meta-tools family.
	TSharedPtr<FJsonObject> ParseBody(const FString& Body)
	{
		const FString Trimmed = Body.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return MakeShared<FJsonObject>();
		}
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			return nullptr;
		}
		return Object;
	}

	FString WriteJson(const TSharedPtr<FJsonValue>& JsonValue)
	{
		if (!JsonValue.IsValid())
		{
			return TEXT("null");
		}
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		if (FJsonSerializer::Serialize(JsonValue, Writer))
		{
			return Out;
		}
		return TEXT("null");
	}

	TSharedRef<FJsonValueArray> ToStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(Values.Num());
		for (const FString& V : Values)
		{
			Items.Add(MakeShared<FJsonValueString>(V));
		}
		return MakeShared<FJsonValueArray>(Items);
	}

	// ---------------------------------------------------------------------
	// Result builders. Mirrors Unity's BuildFixListResult / BuildDryRunResult /
	// BuildApplyResult / BuildUnknownFixError byte-for-byte (the payload
	// schema is a shared contract).
	// ---------------------------------------------------------------------

	FString BuildFixListResult(const FString& IssueId, const TArray<FString>& AvailableFixIds)
	{
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetBoolField(TEXT("dryRun"), true);
		Payload->SetStringField(TEXT("issueId"), IssueId);
		Payload->SetArrayField(TEXT("availableFixIds"), ToStringArray(AvailableFixIds)->AsArray());
		return WriteJson(MakeShared<FJsonValueObject>(Payload));
	}

	FString BuildDryRunResult(const FFixDescription& Desc)
	{
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetBoolField(TEXT("dryRun"), true);
		Payload->SetStringField(TEXT("fixId"), Desc.FixId);
		Payload->SetStringField(TEXT("issueId"), Desc.IssueId);
		Payload->SetStringField(TEXT("assetPath"), Desc.AssetPath);
		Payload->SetStringField(TEXT("description"), Desc.Description);
		Payload->SetBoolField(TEXT("safe"), Desc.bSafe);
		return WriteJson(MakeShared<FJsonValueObject>(Payload));
	}

	FString BuildApplyResult(const FFixResult& Result)
	{
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetBoolField(TEXT("dryRun"), false);
		Payload->SetBoolField(TEXT("success"), true);
		Payload->SetStringField(TEXT("description"), Result.Description);
		Payload->SetArrayField(TEXT("touchedPaths"), ToStringArray(Result.TouchedPaths)->AsArray());
		return WriteJson(MakeShared<FJsonValueObject>(Payload));
	}

	FString BuildUnknownFixError(const FString& FixId, const FString& IssueId)
	{
		const TArray<FString> Available = FFixProviderRegistry::AvailableFixIds();
		const TArray<FString> Applicable = FFixProviderRegistry::FixesForIssue(IssueId);

		TSharedRef<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Inner = MakeShared<FJsonObject>();
		Inner->SetStringField(TEXT("code"), TEXT("unknown_fix"));
		Inner->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Unknown fix id '%s'."), *FixId));
		Inner->SetArrayField(TEXT("availableFixIds"), ToStringArray(Available)->AsArray());
		Inner->SetArrayField(TEXT("applicableFixIdsForIssue"), ToStringArray(Applicable)->AsArray());
		ErrorObj->SetObjectField(TEXT("error"), Inner);
		return WriteJson(MakeShared<FJsonValueObject>(ErrorObj));
	}
} // namespace

bool FUnrealOpenMcpApplyFixTool::IsRollbackSnapshotActive()
{
	return bRollbackSnapshotActive;
}

FUnrealOpenMcpToolDispatchResult FUnrealOpenMcpApplyFixTool::Execute(const FString& Body)
{
	TSharedPtr<FJsonObject> Args = ParseBody(Body);
	if (!Args.IsValid())
	{
		return FUnrealOpenMcpToolDispatchResult::Fail(
			TEXT("invalid_parameter"),
			TEXT("Request body was not a valid JSON object."));
	}

	const FString FixId = Args->HasTypedField<EJson::String>(TEXT("fix_id"))
		? Args->GetStringField(TEXT("fix_id"))
		: FString();
	const FString IssueId = Args->HasTypedField<EJson::String>(TEXT("issue_id"))
		? Args->GetStringField(TEXT("issue_id"))
		: FString();
	const bool bDryRun = Args->HasTypedField<EJson::Bool>(TEXT("dry_run"))
		? Args->GetBoolField(TEXT("dry_run"))
		: true;

	if (IssueId.IsEmpty())
	{
		return FUnrealOpenMcpToolDispatchResult::Fail(
			TEXT("missing_parameter"),
			TEXT("'issue_id' is required and must be non-empty."));
	}

	// Validate the issue key shape. A malformed key is rejected up front so a
	// provider's CanFix never sees garbage input.
	FString ParsedRuleId;
	EVerifySeverity ParsedSeverity = EVerifySeverity::Warning;
	FString ParsedAssetPath;
	FString ParsedIssueCode;
	if (!FIssueKey::TryParse(IssueId, ParsedRuleId, ParsedSeverity, ParsedAssetPath, ParsedIssueCode))
	{
		return FUnrealOpenMcpToolDispatchResult::Fail(
			TEXT("invalid_issue_id"),
			FString::Printf(
				TEXT("Issue id '%s' is not a valid issue key. Expected format: {ruleId}|{severity}|{assetPath}|{issueCode}"),
				*IssueId));
	}

	// fix_id omitted → list every fix that can resolve the issue (safe + unsafe)
	// so the agent can pick. Mirrors Unity's per-issue fix listing.
	if (FixId.IsEmpty())
	{
		const TArray<FString> Available = FFixProviderRegistry::FixesForIssue(IssueId);
		return FUnrealOpenMcpToolDispatchResult::Ok(BuildFixListResult(IssueId, Available));
	}

	IFixProvider* Provider = FFixProviderRegistry::Find(FixId);
	if (Provider == nullptr)
	{
		// Unknown fix id — return Ok with a structured error payload so the
		// envelope builder does not wrap a hard error (matches Unity parity;
		// an agent can read availableFixIds from the body without an isError
		// hop).
		return FUnrealOpenMcpToolDispatchResult::Ok(BuildUnknownFixError(FixId, IssueId));
	}

	if (!Provider->CanFix(IssueId))
	{
		return FUnrealOpenMcpToolDispatchResult::Fail(
			TEXT("fix_not_applicable"),
			FString::Printf(
				TEXT("Fix '%s' cannot be applied to issue '%s'."),
				*FixId, *IssueId));
	}

	if (bDryRun)
	{
		const FFixDescription Desc = Provider->Describe(IssueId);
		return FUnrealOpenMcpToolDispatchResult::Ok(BuildDryRunResult(Desc));
	}

	// Non-dry-run apply MUST run inside the gate runner so a FixRollback
	// snapshot is active. Direct dispatch (no runner wrapper) bypasses the
	// rollback protection and a corrupting fix would be permanent.
	if (!bRollbackSnapshotActive)
	{
		return FUnrealOpenMcpToolDispatchResult::Fail(
			TEXT("rollback_unavailable"),
			TEXT("A non-dry-run apply_fix must run through the gate runner so a "
				 "rollback snapshot protects the asset. Use apply_fix as a top-level "
				 "call (not inside batch_execute) with gate != off, or use dry_run: true "
				 "to preview the fix without mutation."));
	}

	FFixResult Result;
#if WITH_EXCEPTIONS
	try
	{
		Result = Provider->Apply(IssueId);
	}
	catch (const std::exception& E)
	{
		return FUnrealOpenMcpToolDispatchResult::Fail(
			TEXT("fix_error"),
			FString::Printf(TEXT("Fix application failed: %s"), UTF8_TO_TCHAR(E.what())));
	}
	catch (...)
	{
		return FUnrealOpenMcpToolDispatchResult::Fail(
			TEXT("fix_error"),
			TEXT("Fix application failed (unknown exception)."));
	}
#else
	Result = Provider->Apply(IssueId);
#endif

	if (!Result.bSuccess)
	{
		return FUnrealOpenMcpToolDispatchResult::Fail(
			TEXT("fix_failed"),
			Result.Description);
	}
	return FUnrealOpenMcpToolDispatchResult::Ok(BuildApplyResult(Result));
}

// ---------------------------------------------------------------------
// Gate runner
// ---------------------------------------------------------------------

namespace
{
	/**
	 * Predict the on-disk files a fix may touch from the issue's asset path.
	 * The issue's asset path is content-relative ("/Game/Foo/Bar.Bar"); we
	 * resolve it to absolute file paths covering the .uasset plus the
	 * companion .uexp / .ubulk blobs UE writes alongside (a soft-pointer
	 * clear can shrink the bulk payload, so all three are snapshotted).
	 *
	 * Returns an empty array when the issue id cannot be parsed or the asset
	 * path is not under /Game/ (source-file issues are not Safe in v1).
	 */
	TArray<FString> PredictTouchedPaths(const FString& IssueId)
	{
		TArray<FString> Out;

		FString RuleId;
		EVerifySeverity Severity = EVerifySeverity::Warning;
		FString AssetPath;
		FString IssueCode;
		if (!FIssueKey::TryParse(IssueId, RuleId, Severity, AssetPath, IssueCode))
		{
			return Out;
		}
		if (AssetPath.IsEmpty())
		{
			return Out;
		}

		// Strip the asset-name tail ("/Game/Foo/Bar.Bar" → "/Game/Foo/Bar").
		FString LongPackageName = AssetPath;
		const int32 LastSlash = LongPackageName.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		const int32 Dot = LongPackageName.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (Dot != INDEX_NONE && (LastSlash == INDEX_NONE || Dot > LastSlash))
		{
			LongPackageName = LongPackageName.Left(Dot);
		}
		if (LongPackageName.IsEmpty() || !LongPackageName.StartsWith(TEXT("/Game/")))
		{
			// Not a content-package path — source files, /Engine/ paths, and
			// the (project) sentinel are out of scope for v1 Safe rollback.
			return Out;
		}

		const FString AbsoluteBase = FPackageName::LongPackageNameToFilename(LongPackageName, FPackageName::GetAssetPackageExtension());
		if (AbsoluteBase.IsEmpty())
		{
			return Out;
		}
		Out.Add(AbsoluteBase);

		// Companion .uexp (export table + payload) — almost always present
		// for any non-trivial asset. Snapshot it when it exists.
		const FString UexpPath = FPackageName::LongPackageNameToFilename(LongPackageName, TEXT(".uexp"));
		if (!UexpPath.IsEmpty())
		{
			Out.Add(UexpPath);
		}
		// Companion .ubulk (bulk data — textures, large arrays). Snapshot it
		// when present; absent for assets without bulk data.
		const FString UbulkPath = FPackageName::LongPackageNameToFilename(LongPackageName, TEXT(".ubulk"));
		if (!UbulkPath.IsEmpty())
		{
			Out.Add(UbulkPath);
		}
		return Out;
	}

	/**
	 * Convert an absolute restored path back into a content-relative form the
	 * rest of the API uses (matches the issue asset path style). Absolute paths
	 * that do not resolve to a content path are returned unchanged so the agent
	 * still sees a meaningful string.
	 */
	TArray<FString> NormalizeRestoredPaths(const TArray<FString>& AbsolutePaths)
	{
		TArray<FString> Out;
		Out.Reserve(AbsolutePaths.Num());
		for (const FString& Path : AbsolutePaths)
		{
			Out.Add(FPackageName::FilenameToLongPackageName(Path));
		}
		return Out;
	}
} // namespace

FUnrealOpenMcpApplyFixGateRunnerResult FUnrealOpenMcpApplyFixGateRunner::Execute(
	const FString& Body,
	EUnrealOpenMcpGateMode Mode,
	const TArray<FString>& PathsHint)
{
	// Extract the issue id so we can predict the rollback scope BEFORE the
	// gate runs the mutation. The body is JSON; a tiny parse is fine (we
	// already pay FJsonObject parse in the inner handler).
	FString IssueId;
	if (TSharedPtr<FJsonObject> Args; true)
	{
		const FString Trimmed = Body.TrimStartAndEnd();
		if (!Trimmed.IsEmpty())
		{
			TSharedPtr<FJsonObject> Object;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
			if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
			{
				if (Object->HasTypedField<EJson::String>(TEXT("issue_id")))
				{
					IssueId = Object->GetStringField(TEXT("issue_id"));
				}
			}
		}
	}

	const TArray<FString> PredictedPaths = PredictTouchedPaths(IssueId);
	FUnrealOpenMcpFixRollback Rollback;
	if (PredictedPaths.Num() > 0)
	{
		Rollback.Snapshot(PredictedPaths);
	}

	// Run the apply inside the gate path. The ambient flag tells the inner
	// handler it is safe to invoke Apply (no rollback_unavailable refusal).
	FUnrealOpenMcpApplyFixGateRunnerResult Out;
	const bool bPreviousFlag = bRollbackSnapshotActive;
	bRollbackSnapshotActive = true;

	FUnrealOpenMcpGateDispatchResult GateResult;
#if WITH_EXCEPTIONS
	try
	{
		GateResult = FUnrealOpenMcpGatePolicy::Execute(Mode, PathsHint,
			[&Body]() -> FUnrealOpenMcpToolDispatchResult
			{
				return FUnrealOpenMcpApplyFixTool::Execute(Body);
			});
	}
	catch (...)
	{
		// Checkpoint / validate blew up (the gate wraps the mutation in a
		// try/catch internally, so this only fires for checkpoint failures).
		// Roll back to be safe, then rethrow so the bridge builds the fault
		// envelope.
		if (Rollback.HasSnapshot())
		{
			Rollback.Restore();
		}
		Rollback.Discard();
		bRollbackSnapshotActive = bPreviousFlag;
		throw;
	}
#else
	GateResult = FUnrealOpenMcpGatePolicy::Execute(Mode, PathsHint,
		[&Body]() -> FUnrealOpenMcpToolDispatchResult
		{
			return FUnrealOpenMcpApplyFixTool::Execute(Body);
		});
#endif
	bRollbackSnapshotActive = bPreviousFlag;

	Out.Gate = MoveTemp(GateResult);

	// Decide whether to roll back. Two triggers (Unity parity):
	//   1. The mutation itself failed (provider returned !bSuccess or threw
	//      before the validate step — gate marks these Outcome=Failed).
	//   2. Under Enforce, the gate detected new errors after the fix. Warn
	//      never rolls back (operator asked for report-only); Off has no
	//      delta to check.
	const bool bMutationFailed = !Out.Gate.Mutation.bOk;
	const bool bGateIntroducedErrors = Mode == EUnrealOpenMcpGateMode::Enforce
		&& Out.Gate.bGateRan
		&& Out.Gate.Delta.IsSet()
		&& Out.Gate.Delta->NewErrors > 0;

	if ((bMutationFailed || bGateIntroducedErrors) && Rollback.HasSnapshot())
	{
		const FUnrealOpenMcpFixRollbackRestore Restore = Rollback.Restore();

		Out.bRolledBack = true;
		Out.RollbackReason = bMutationFailed
			? TEXT("fix failed to apply — restored touched files to pre-fix state")
			: FString::Printf(
				TEXT("fix introduced %d new error(s) under enforce — restored touched files to pre-fix state"),
				Out.Gate.Delta->NewErrors);
		Out.RestoredPaths = NormalizeRestoredPaths(Restore.RestoredPaths);

		Out.Gate.AgentNextSteps.Add(TEXT(
			"The fix was rolled back — no project change remains. Inspect the issue manually before retrying."));
	}
	else if (Mode == EUnrealOpenMcpGateMode::Off && Out.Gate.Mutation.bOk)
	{
		// gate:"off" skips the delta and never consults the rollback snapshot.
		// A fix that corrupts the asset under this path is permanent (no auto-
		// restore). Surface a structured rollbackDisabled warning so the agent
		// knows the mutation committed without rollback protection and can
		// verify health manually. The operator explicitly asked for no gate,
		// so this is a warning, not a refusal.
		Out.bRollbackDisabled = true;
		Out.Gate.AgentNextSteps.Add(TEXT(
			"The fix was applied with gate:\"off\" — no rollback snapshot was consulted. "
			"If the asset is in a bad state, run unreal_open_mcp_validate_edit (or "
			"unreal_open_mcp_scan_paths) on the touched path to confirm health."));
	}

	Rollback.Discard();
	return Out;
}

// ---------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------

void FUnrealOpenMcpApplyFixMetaTools::Register(FUnrealOpenMcpToolRegistry& Registry)
{
	// Mutating tool — default gate Enforce. The dispatcher short-circuits
	// dry_run=true directly to the inner handler (no checkpoint/validate
	// around a preview that mutates nothing); non-dry-run applies route
	// through FUnrealOpenMcpApplyFixGateRunner (see HandleToolDispatch).
	Registry.Register(
		TEXT("unreal_open_mcp_apply_fix"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			return FUnrealOpenMcpApplyFixTool::Execute(Body);
		},
		FUnrealOpenMcpToolMetadata::Mutating(EUnrealOpenMcpGateMode::Enforce));
}
