// Gate meta-tool family — see header for the validate_edit / checkpoint_create
// / delta contract. This file owns the three handlers and the shared JSON
// helpers (ParseBody / WriteJson / severity token / issue serializer).
//
// Ported from Unity Open MCP packages/bridge/Editor/MetaTools/
// (ValidateEditTool.cs / CheckpointCreateTool.cs / DeltaTool.cs) at copy
// fidelity:
//   - Validate_edit / checkpoint_create / delta semantics are byte-for-byte
//     ports (the issue payload schema, the fingerprint summary shape, and the
//     unavailable/lost-on-reload recovery payloads are all shared contracts
//     with the Unity tools — agents that consume one consume the other).
//   - The C++ handlers use FJsonObject + FJsonSerializer instead of Unity's
//     StringBuilder; the resulting JSON is equivalent (verified by the spec
//     that round-trips the JSON through a parser to assert field presence).
//   - The unavailable / lost-on-reload payloads carry the same `agentNextSteps`
//     guidance strings as Unity so an agent prompt that works against one
//     works against the other.
//
// Arg parsing: each handler receives the raw POST body FString (the registry
// contract is raw-body; each tool owns its arg extraction). The handlers parse
// the body into an FJsonObject via the Json module (TJsonReader +
// FJsonSerializer).
#include "MetaTools/UnrealOpenMcpGateMetaTools.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Gate/UnrealOpenMcpCheckpointStore.h"
#include "Gate/UnrealOpenMcpVerifyGateAdapter.h"

#include "Core/CheckpointFingerprint.h"
#include "Core/VerifyIssue.h"
#include "Core/VerifyResult.h"
#include "Core/VerifySeverity.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "Misc/DateTime.h"

namespace
{
	/**
	 * Parse the raw POST body into a JSON object. Returns null when the body is
	 * not a JSON object (caller surfaces a structured `invalid_parameter`
	 * error). Empty body → empty object so handlers can resolve optional
	 * fields with their defaults. Copied from the actor-tool family's ParseBody
	 * so the parsing contract is identical across the bridge.
	 */
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

	/** Serialize a JsonValue to a compact string. Returns "null" on a null
	 *  pointer so the result is always valid JSON. */
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

	/** Read a string array field, tolerating absence (returns empty array) and
	 *  wrong type (returns empty array rather than erroring). Used by every
	 *  meta-tool that accepts an optional paths / categories / rules array. */
	TArray<FString> ReadStringArray(const TSharedPtr<FJsonObject>& Args, const FString& FieldName)
	{
		TArray<FString> Out;
		if (!Args.IsValid() || !Args->HasTypedField<EJson::Array>(FieldName))
		{
			return Out;
		}
		const TArray<TSharedPtr<FJsonValue>>& Raw = Args->GetArrayField(FieldName);
		Out.Reserve(Raw.Num());
		for (const TSharedPtr<FJsonValue>& V : Raw)
		{
			if (V.IsValid() && V->Type == EJson::String)
			{
				Out.Add(V->AsString());
			}
		}
		return Out;
	}

	/** Stable severity token emitted in issue payloads. Mirrors Unity's
	 *  SeverityStr switch ("Error" / "Warning" / "Info"). Verify's v1 set is
	 *  Error / Warning; the Info branch covers a future severity without
	 *  breaking the contract. */
	const TCHAR* SeverityToken(EVerifySeverity Severity)
	{
		switch (Severity)
		{
			case EVerifySeverity::Error: return TEXT("Error");
			case EVerifySeverity::Warning: return TEXT("Warning");
			default: return TEXT("Info");
		}
	}

	/** Build one issue object in the canonical shape agents consume. Mirrors
	 *  Unity's ValidateEditTool.BuildResult per-issue object: ruleId +
	 *  categoryId (alias of ruleId) + severity + code + issueCode (alias of
	 *  code) + assetPath + description + evidence (optional). The
	 *  rootCause / remediation / fixCandidates / fixId / fixSafe fields land
	 *  with the explainability table + apply_fix (P3.7) — omitted here. */
	TSharedRef<FJsonObject> BuildIssueObject(const FVerifyIssue& Issue)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		// categoryId mirrors ruleId so agents can match the capability
		// catalog field (T2.6). Pinned by Unity's contract.
		Obj->SetStringField(TEXT("ruleId"), Issue.RuleId);
		Obj->SetStringField(TEXT("categoryId"), Issue.RuleId);
		Obj->SetStringField(TEXT("severity"), SeverityToken(Issue.Severity));
		// code + issueCode are aliases (issueCode is the T2.6 name; code is
		// the legacy alias Unity keeps for back-compat).
		Obj->SetStringField(TEXT("code"), Issue.IssueCode);
		Obj->SetStringField(TEXT("issueCode"), Issue.IssueCode);
		Obj->SetStringField(TEXT("assetPath"), Issue.AssetPath);
		Obj->SetStringField(TEXT("description"), Issue.Description);

		if (Issue.Evidence.Num() > 0)
		{
			TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
			for (const auto& Pair : Issue.Evidence)
			{
				Evidence->SetStringField(Pair.Key, Pair.Value);
			}
			Obj->SetObjectField(TEXT("evidence"), Evidence);
		}
		return Obj;
	}

	/** Build a JSON array of strings from a TArray<FString>. */
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

	// Forward declarations of the unavailable / lost-on-reload payload builders
	// (defined below ExecuteDelta — they are referenced by the missing-
	// checkpoint path before they appear textually). Mirrors Unity's
	// private static methods, which the C# compiler hoists for us.
	TSharedRef<FJsonObject> BuildUnavailableResult(const FString& CheckpointId);
	TSharedRef<FJsonObject> BuildLostOnReloadResult(const FString& CheckpointId);

	//----------------------------------------------------------------------
	// validate_edit
	//----------------------------------------------------------------------
	FUnrealOpenMcpToolDispatchResult ExecuteValidateEdit(const FString& Body)
	{
		TSharedPtr<FJsonObject> Args = ParseBody(Body);
		if (!Args.IsValid())
		{
			return FUnrealOpenMcpToolDispatchResult::Fail(
				TEXT("invalid_parameter"),
				TEXT("Request body was not a valid JSON object."));
		}

		// `paths` is required. Mirrors Unity's contract: validate_edit answers
		// "is this asset currently healthy?" and needs at least one path to
		// scope the run.
		const TArray<FString> Paths = ReadStringArray(Args, TEXT("paths"));
		if (Paths.Num() == 0)
		{
			return FUnrealOpenMcpToolDispatchResult::Fail(
				TEXT("missing_parameter"),
				TEXT("'paths' is required and must be a non-empty array."));
		}
		const TArray<FString> Categories = ReadStringArray(Args, TEXT("categories"));
		const TArray<FString> IncludeRules = ReadStringArray(Args, TEXT("include_rules"));
		const TArray<FString> ExcludeRules = ReadStringArray(Args, TEXT("exclude_rules"));

		// Resolve + filter the rule set via the gate adapter (the same surface
		// the apply_fix runner will consult in P3.7). Filters that reduce the
		// set to nothing return an explicit empty result.
		const FUnrealOpenMcpFilteredVerifyResult Filtered =
			FUnrealOpenMcpVerifyGateAdapter::ValidateFiltered(Paths, Categories, IncludeRules, ExcludeRules);
		const FVerifyResult& Result = Filtered.Result;

		// Unknown-rule path: when the caller asked for a rule id that is not
		// registered, surface it as a structured error with the available
		// roster so the agent can self-correct (Unity parity).
		if (Result.HasUnknownRules())
		{
			TSharedRef<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> Inner = MakeShared<FJsonObject>();
			Inner->SetStringField(TEXT("code"), TEXT("unknown_rule"));
			const FString UnknownJoined = FString::Join(Result.UnknownRuleIds, TEXT(", "));
			Inner->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Unknown rule IDs: %s"), *UnknownJoined));
			Inner->SetArrayField(TEXT("unknownRules"), ToStringArray(Result.UnknownRuleIds)->AsArray());
			Inner->SetArrayField(TEXT("availableRules"), ToStringArray(Result.AvailableRuleIds)->AsArray());
			ErrorObj->SetObjectField(TEXT("error"), Inner);
			return FUnrealOpenMcpToolDispatchResult::Ok(WriteJson(MakeShared<FJsonValueObject>(ErrorObj)));
		}

		// `passed` is the strict-error contract: validate_edit is the gate's
		// pre-mutation check; it fails on any Error. The project severity
		// threshold flows into scan_paths and the regression gate; here the
		// contract is strict-error because validate_edit answers "is this
		// asset currently healthy?". (Copied from Unity's BuildResult
		// preamble.)
		bool bHasErrors = false;
		for (const FVerifyIssue& Issue : Result.Issues)
		{
			if (Issue.Severity == EVerifySeverity::Error)
			{
				bHasErrors = true;
				break;
			}
		}

		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetBoolField(TEXT("passed"), !bHasErrors);

		TArray<TSharedPtr<FJsonValue>> Issues;
		Issues.Reserve(Result.Issues.Num());
		for (const FVerifyIssue& Issue : Result.Issues)
		{
			Issues.Add(MakeShared<FJsonValueObject>(BuildIssueObject(Issue)));
		}
		Payload->SetArrayField(TEXT("issues"), Issues);

		Payload->SetArrayField(TEXT("categoriesRun"), ToStringArray(Result.CategoriesRun)->AsArray());
		Payload->SetArrayField(TEXT("rulesApplied"), ToStringArray(Filtered.RulesApplied)->AsArray());
		Payload->SetNumberField(TEXT("durationMs"), static_cast<double>(Result.DurationMs));

		return FUnrealOpenMcpToolDispatchResult::Ok(WriteJson(MakeShared<FJsonValueObject>(Payload)));
	}

	//----------------------------------------------------------------------
	// checkpoint_create
	//----------------------------------------------------------------------
	FUnrealOpenMcpToolDispatchResult ExecuteCheckpointCreate(const FString& Body)
	{
		TSharedPtr<FJsonObject> Args = ParseBody(Body);
		if (!Args.IsValid())
		{
			return FUnrealOpenMcpToolDispatchResult::Fail(
				TEXT("invalid_parameter"),
				TEXT("Request body was not a valid JSON object."));
		}

		const TArray<FString> Paths = ReadStringArray(Args, TEXT("paths"));
		const FString Label = Args->HasTypedField<EJson::String>(TEXT("label"))
			? Args->GetStringField(TEXT("label"))
			: FString();
		const TArray<FString> Categories = ReadStringArray(Args, TEXT("categories"));

		// Capture the fingerprint. Empty Categories → the adapter falls back
		// to SelectRuleIds(paths) (or every registered rule when paths is
		// empty). Mirrors Unity's CreateCheckpoint(paths, ruleIds ?? SelectRuleIds).
		const FCheckpointFingerprint Checkpoint = Categories.Num() > 0
			? FUnrealOpenMcpVerifyGateAdapter::CreateCheckpoint(Paths, Categories)
			: FUnrealOpenMcpVerifyGateAdapter::CreateCheckpoint(Paths);

		// Mirror into the session checkpoint store so the delta meta-tool can
		// find it. The store is process-lifetime; see
		// Gate/UnrealOpenMcpCheckpointStore.h for the storage contract.
		FUnrealOpenMcpCheckpointStoreEntry Entry;
		Entry.CheckpointId = Checkpoint.CheckpointId;
		Entry.TimestampUtc = FDateTime::UtcNow().ToIso8601();
		Entry.Label = Label;
		Entry.Paths = Paths;
		// Categories run = the rule ids in the fingerprint (the runner
		// produces a fingerprint per category it actually invoked).
		Entry.Categories.Reset();
		for (const auto& Pair : Checkpoint.Fingerprints)
		{
			Entry.Categories.Add(Pair.Key);
		}
		Entry.Fingerprint = Checkpoint;
		// Capture the timestamp before MoveTemp — the Store() call moves
		// every field out of Entry, so the post-call fallback below needs a
		// copy.
		const FString TimestampUtc = Entry.TimestampUtc;
		FUnrealOpenMcpCheckpointStore::Store(MoveTemp(Entry));

		// Build the result payload: {checkpointId, timestamp, fingerprint{
		//   <ruleId>:{errors,warnings,issueKeys:[...]}}}. Mirrors Unity's
		// BuildResult shape.
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		const FUnrealOpenMcpCheckpointStoreEntry* Stored = FUnrealOpenMcpCheckpointStore::Get(Checkpoint.CheckpointId);
		// Get() bumps recency; we just stored it so the lookup cannot miss
		// unless the store was concurrently cleared (test helper). Fall back
		// to the captured timestamp when that happens so the response is
		// always populated.
		Payload->SetStringField(TEXT("checkpointId"), Checkpoint.CheckpointId);
		Payload->SetStringField(TEXT("timestamp"),
			(Stored != nullptr) ? Stored->TimestampUtc : TimestampUtc);

		TSharedRef<FJsonObject> FingerprintObj = MakeShared<FJsonObject>();
		for (const auto& Pair : Checkpoint.Fingerprints)
		{
			TSharedRef<FJsonObject> RuleFp = MakeShared<FJsonObject>();
			RuleFp->SetNumberField(TEXT("errors"), static_cast<double>(Pair.Value.Errors));
			RuleFp->SetNumberField(TEXT("warnings"), static_cast<double>(Pair.Value.Warnings));

			TArray<FString> Keys;
			Pair.Value.IssueKeys.GetKeys(Keys);
			Keys.Sort();
			RuleFp->SetArrayField(TEXT("issueKeys"), ToStringArray(Keys)->AsArray());

			FingerprintObj->SetObjectField(Pair.Key, RuleFp);
		}
		Payload->SetObjectField(TEXT("fingerprint"), FingerprintObj);

		return FUnrealOpenMcpToolDispatchResult::Ok(WriteJson(MakeShared<FJsonValueObject>(Payload)));
	}

	//----------------------------------------------------------------------
	// delta
	//----------------------------------------------------------------------
	FUnrealOpenMcpToolDispatchResult ExecuteDelta(const FString& Body)
	{
		TSharedPtr<FJsonObject> Args = ParseBody(Body);
		if (!Args.IsValid())
		{
			return FUnrealOpenMcpToolDispatchResult::Fail(
				TEXT("invalid_parameter"),
				TEXT("Request body was not a valid JSON object."));
		}

		const FString CheckpointId = Args->HasTypedField<EJson::String>(TEXT("checkpoint_id"))
			? Args->GetStringField(TEXT("checkpoint_id"))
			: FString();
		if (CheckpointId.IsEmpty())
		{
			return FUnrealOpenMcpToolDispatchResult::Fail(
				TEXT("missing_parameter"),
				TEXT("'checkpoint_id' is required."));
		}

		const FUnrealOpenMcpCheckpointStoreEntry* Stored = FUnrealOpenMcpCheckpointStore::Get(CheckpointId);
		if (Stored == nullptr)
		{
			// A missing checkpoint is NOT a tool failure: checkpoints are
			// session-scoped (in-memory) and are wiped on hot reload or editor
			// restart. Returning a hard error would set isError:true on the MCP
			// response and block agent workflows. Instead return success with
			// an explicit `unavailable` warning + recovery guidance so the
			// agent can proceed (e.g. fall back to validate_edit).
			//
			// T5.4 — when the store is completely empty AND a specific id was
			// requested, the most likely cause is a hot reload that wiped the
			// in-memory store. Surface a distinct `checkpointLostOnReload`
			// flag so the agent can distinguish "wiped by reload" from "this
			// id was never created in this session".
			const bool bStoreEmpty = FUnrealOpenMcpCheckpointStore::Count() == 0;
			return FUnrealOpenMcpToolDispatchResult::Ok(
				bStoreEmpty
					? BuildLostOnReloadResult(CheckpointId)
					: BuildUnavailableResult(CheckpointId));
		}

		// Re-validate over the stored paths (or the request's `paths` override).
		const TArray<FString> OverridePaths = ReadStringArray(Args, TEXT("paths"));
		const TArray<FString>& Paths = OverridePaths.Num() > 0 ? OverridePaths : Stored->Paths;

		const FVerifyResult Current = FUnrealOpenMcpVerifyGateAdapter::ValidatePaths(Paths, Stored->Categories);
		const FUnrealOpenMcpGateDelta Delta = FUnrealOpenMcpVerifyGateAdapter::ComputeDelta(Stored->Fingerprint, Current);

		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetBoolField(TEXT("passed"), Delta.NewErrors == 0);

		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetNumberField(TEXT("newErrors"), static_cast<double>(Delta.NewErrors));
		Summary->SetNumberField(TEXT("newWarnings"), static_cast<double>(Delta.NewWarnings));
		Summary->SetNumberField(TEXT("resolvedErrors"), static_cast<double>(Delta.ResolvedErrors));
		Summary->SetNumberField(TEXT("resolvedWarnings"), static_cast<double>(Delta.ResolvedWarnings));
		Payload->SetObjectField(TEXT("summary"), Summary);

		Payload->SetArrayField(TEXT("newIssues"), ToStringArray(Delta.NewIssueKeys)->AsArray());
		Payload->SetArrayField(TEXT("resolvedIssues"), ToStringArray(Delta.ResolvedIssueKeys)->AsArray());

		return FUnrealOpenMcpToolDispatchResult::Ok(WriteJson(MakeShared<FJsonValueObject>(Payload)));
	}

	/** Payload returned when the requested checkpoint is no longer in the
	 *  session-scoped store. `passed:true` + `unavailable:true` lets the agent
	 *  treat this as "no new errors detected, but I have no baseline to delta
	 *  against" rather than a hard failure. Mirrors Unity's BuildUnavailableResult
	 *  byte-for-byte (the agentNextSteps guidance is the same shared contract). */
	TSharedRef<FJsonObject> BuildUnavailableResult(const FString& CheckpointId)
	{
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetBoolField(TEXT("passed"), true);
		Payload->SetBoolField(TEXT("unavailable"), true);
		Payload->SetStringField(TEXT("warning"),
			FString::Printf(
				TEXT("Checkpoint '%s' is no longer available. Checkpoints are session-scoped (in-memory) and are cleared on script recompile, hot reload, or editor restart — this does not indicate a problem with the project."),
				*CheckpointId));
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Add(MakeShared<FJsonValueString>(TEXT("The pre-change baseline is gone, so a delta cannot be computed.")));
		Steps.Add(MakeShared<FJsonValueString>(TEXT("To verify current state directly, call unreal_open_mcp_validate_edit (or unreal_open_mcp_scan_paths) on the relevant paths.")));
		Steps.Add(MakeShared<FJsonValueString>(TEXT("For future delta checks, call unreal_open_mcp_checkpoint_create immediately before mutating, then unreal_open_mcp_delta right after.")));
		Payload->SetArrayField(TEXT("agentNextSteps"), Steps);
		return Payload;
	}

	/** Payload returned when the store is empty AND a specific id was
	 *  requested. The empty-store signal alone cannot distinguish "wiped by a
	 *  hot reload" from "no checkpoint was ever created this session" (the
	 *  store tracks no process-lifetime marker), so the wording covers both.
	 *  The most likely cause is a hot reload triggered by the gate flow itself
	 *  (a recompile that follows a C++ edit) or an editor restart. Distinct
	 *  from BuildUnavailableResult (which fires when the id is unknown but
	 *  other checkpoints still exist). Mirrors Unity's BuildLostOnReloadResult. */
	TSharedRef<FJsonObject> BuildLostOnReloadResult(const FString& CheckpointId)
	{
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetBoolField(TEXT("passed"), true);
		Payload->SetBoolField(TEXT("unavailable"), true);
		Payload->SetBoolField(TEXT("checkpointLostOnReload"), true);
		Payload->SetStringField(TEXT("warning"),
			FString::Printf(
				TEXT("Checkpoint '%s' was not found and the in-memory checkpoint store is empty. The most likely cause is a hot reload (script recompile, plugin edit) or editor restart wiping the process-lifetime store. It is also possible no checkpoint has been created yet this session. Either way a delta cannot be computed. This does not indicate a problem with the project."),
				*CheckpointId));
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Add(MakeShared<FJsonValueString>(TEXT("The checkpoint store is empty — the pre-change baseline is gone (or was never created) and a delta cannot be computed.")));
		Steps.Add(MakeShared<FJsonValueString>(TEXT("To verify current state directly, call unreal_open_mcp_validate_edit (or unreal_open_mcp_scan_paths) on the relevant paths.")));
		Steps.Add(MakeShared<FJsonValueString>(TEXT("To re-establish a baseline, call unreal_open_mcp_checkpoint_create on the paths you intend to delta-check, then mutate, then unreal_open_mcp_delta.")));
		Payload->SetArrayField(TEXT("agentNextSteps"), Steps);
		return Payload;
	}
} // namespace

void FUnrealOpenMcpGateMetaTools::Register(FUnrealOpenMcpToolRegistry& Registry)
{
	// validate_edit — scoped health check without a preceding mutation. Used
	// by agents for manual verification or pre-commit checks. Auto-selects
	// rules by extension; the caller narrows via categories / include_rules /
	// exclude_rules. Read-only: bypasses GatePolicy.Execute (the meta-tool
	// participates in the gate workflow but must not recurse through it).
	Registry.Register(
		TEXT("unreal_open_mcp_validate_edit"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			return ExecuteValidateEdit(Body);
		});

	// checkpoint_create — capture a fingerprint for later delta comparison.
	// Stores the entry in FUnrealOpenMcpCheckpointStore so a subsequent
	// delta call can diff against it without re-running the pre-mutation
	// snapshot. Read-only.
	Registry.Register(
		TEXT("unreal_open_mcp_checkpoint_create"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			return ExecuteCheckpointCreate(Body);
		});

	// delta — compare current project health vs a stored checkpoint.
	// Surfaces added / resolved issues; a missing checkpoint returns
	// structured recovery guidance (unavailable / checkpointLostOnReload)
	// rather than a hard error. Read-only.
	Registry.Register(
		TEXT("unreal_open_mcp_delta"),
		[](const FString& Body) -> FUnrealOpenMcpToolDispatchResult
		{
			return ExecuteDelta(Body);
		});
}
