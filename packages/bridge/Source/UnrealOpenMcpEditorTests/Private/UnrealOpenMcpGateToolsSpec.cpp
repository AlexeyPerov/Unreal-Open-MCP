// Gate meta-tool Automation specs (P3.6).
//
// Pins the three gate meta-tool handlers end-to-end. Three surfaces:
//   1. validate_edit — happy path (stub rules → expected issues), unknown-rule
//      path (structured unknown_rule error), missing-parameter path.
//   2. checkpoint_create — happy path (returns checkpointId + fingerprint
//      summary), stored entry is retrievable.
//   3. delta — checkpoint → mutate-simulate → delta happy path; missing-
//      checkpoint path (structured unavailable / lost-on-reload); missing-
//      parameter path.
//
// The meta-tools are pure data transforms over the verify runner + checkpoint
// store, so the spec drives the registered handlers directly via a fresh
// FUnrealOpenMcpToolRegistry (no HTTP hop). The stub rules emit deterministic
// issues over a fixture content path so the delta math has a known shape.
//
// Adapted from Unity's ValidateEdit / CheckpointCreate / Delta meta-tool tests
// at copy fidelity (the issue payload schema, the fingerprint summary shape,
// the unavailable / lost-on-reload recovery payloads are all shared contracts
// pinned here).
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Gate/UnrealOpenMcpCheckpointStore.h"
#include "Gate/UnrealOpenMcpVerifyGateAdapter.h"
#include "MetaTools/UnrealOpenMcpGateMetaTools.h"

#include "Core/IVerifyRule.h"
#include "Core/IssueKey.h"
#include "Core/VerifyIssue.h"
#include "Core/VerifyRunMode.h"
#include "Core/VerifyRunner.h"
#include "Core/VerifyScope.h"
#include "Core/VerifySeverity.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpGateToolsSpec,
	"UnrealOpenMcp.Tools.GateMetaTools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpGateToolsSpec)

namespace
{
	/** Fixture content path used by every stub rule so IssueKey has a stable
	 *  AssetPath component. Does NOT need to exist on disk — the stub rules do
	 *  not consult the asset registry. */
	constexpr const TCHAR* FixturePath = TEXT("/Game/Fixtures/GateMetaToolsTest.uasset");

	/** Parse a JSON object from a string. Null on failure. Mirrors the helper
	 *  in the actor-create spec so the parsing contract is identical. */
	TSharedPtr<FJsonObject> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	/** Stub rule that emits exactly one Error per scan. */
	class FErrorRule : public IVerifyRule
	{
	public:
		FString Id;
		explicit FErrorRule(FString InId) : Id(MoveTemp(InId)) {}
		virtual FString GetId() const override { return Id; }
		virtual void Scan(const FVerifyScope& Scope, const EVerifyRunMode Mode, TArray<FVerifyIssue>& Sink) const override
		{
			Sink.Emplace(Id, EVerifySeverity::Error, FixturePath, TEXT("gate_meta_error"), TEXT("gate meta spec error"));
		}
	};

	/** Stub rule that emits one Warning per scan. */
	class FWarningRule : public IVerifyRule
	{
	public:
		FString Id;
		explicit FWarningRule(FString InId) : Id(MoveTemp(InId)) {}
		virtual FString GetId() const override { return Id; }
		virtual void Scan(const FVerifyScope& Scope, const EVerifyRunMode Mode, TArray<FVerifyIssue>& Sink) const override
		{
			Sink.Emplace(Id, EVerifySeverity::Warning, FixturePath, TEXT("gate_meta_warn"), TEXT("gate meta spec warning"));
		}
	};

	/** RAII guard: clear the rule registry + checkpoint store on construct /
	 *  destruct so each spec sees a clean slate. Both are process-global
	 *  static state. */
	struct FScopedState
	{
		FScopedState()
		{
			FVerifyRunner::ClearRules();
			FUnrealOpenMcpCheckpointStore::Clear();
		}
		~FScopedState()
		{
			FVerifyRunner::ClearRules();
			FUnrealOpenMcpCheckpointStore::Clear();
		}
	};

	TArray<FString> Paths()
	{
		return {FixturePath};
	}
} // namespace

void FUnrealOpenMcpGateToolsSpec::Define()
{
	Describe("unreal_open_mcp_validate_edit — handler contract", [this]()
	{
		It("returns the issue list with stable code/severity fields", [this]()
		{
			FScopedState Guard;
			FVerifyRunner::RegisterRule(MakeUnique<FErrorRule>(TEXT("stub_errors")));
			FVerifyRunner::RegisterRule(MakeUnique<FWarningRule>(TEXT("stub_warnings")));

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpGateMetaTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), Registry.TryGet(TEXT("unreal_open_mcp_validate_edit"), Handler));

			const FString Body = FString::Printf(TEXT("{\"paths\":[\"%s\"]}"), FixturePath);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			// passed is false because the stub error rule fired.
			TestEqual(TEXT("passed false"), Json->GetBoolField(TEXT("passed")), false);
			// issues array carries both rules' findings.
			const TArray<TSharedPtr<FJsonValue>>& Issues = Json->GetArrayField(TEXT("issues"));
			TestEqual(TEXT("issue count"), Issues.Num(), 2);
			// rulesApplied lists both stub rule Ids.
			const TArray<TSharedPtr<FJsonValue>>& RulesApplied = Json->GetArrayField(TEXT("rulesApplied"));
			TestEqual(TEXT("rulesApplied count"), RulesApplied.Num(), 2);
		});

		It("returns unknown_rule error when a requested rule is not registered", [this]()
		{
			FScopedState Guard;
			FVerifyRunner::RegisterRule(MakeUnique<FErrorRule>(TEXT("stub_errors")));

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpGateMetaTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_validate_edit"), Handler);

			// Request a rule id that does not exist alongside the real one.
			const FString Body = FString::Printf(
				TEXT("{\"paths\":[\"%s\"],\"categories\":[\"stub_errors\",\"does_not_exist\"]}"),
				FixturePath);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
			TestTrue(TEXT("error field"), Json->TryGetObjectField(TEXT("error"), ErrorObj));
			if (ErrorObj == nullptr || !ErrorObj->IsValid())
			{
				return;
			}
			TestEqual(TEXT("code"), (*ErrorObj)->GetStringField(TEXT("code")), FString(TEXT("unknown_rule")));
			const TArray<TSharedPtr<FJsonValue>>& Unknown = (*ErrorObj)->GetArrayField(TEXT("unknownRules"));
			TestEqual(TEXT("one unknown id"), Unknown.Num(), 1);
			// availableRules carries the registered roster for self-correction.
			const TArray<TSharedPtr<FJsonValue>>& Available = (*ErrorObj)->GetArrayField(TEXT("availableRules"));
			TestEqual(TEXT("one available id"), Available.Num(), 1);
		});

		It("fails fast on missing_parameter when paths is absent", [this]()
		{
			FScopedState Guard;
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpGateMetaTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_validate_edit"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});
	});

	Describe("unreal_open_mcp_checkpoint_create — handler contract", [this]()
	{
		It("captures a fingerprint and stores it in the checkpoint store", [this]()
		{
			FScopedState Guard;
			FVerifyRunner::RegisterRule(MakeUnique<FErrorRule>(TEXT("stub_errors")));

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpGateMetaTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			TestTrue(TEXT("handler registered"), Registry.TryGet(TEXT("unreal_open_mcp_checkpoint_create"), Handler));

			const FString Body = FString::Printf(
				TEXT("{\"paths\":[\"%s\"],\"label\":\"before-mutate\"}"), FixturePath);
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			const FString CheckpointId = Json->GetStringField(TEXT("checkpointId"));
			TestTrue(TEXT("checkpointId set"), !CheckpointId.IsEmpty());
			TestTrue(TEXT("timestamp set"), !Json->GetStringField(TEXT("timestamp")).IsEmpty());
			// fingerprint object carries one entry per registered rule.
			const TSharedPtr<FJsonObject>* FingerprintObj = nullptr;
			TestTrue(TEXT("fingerprint field"), Json->TryGetObjectField(TEXT("fingerprint"), FingerprintObj));
			if (FingerprintObj == nullptr || !FingerprintObj->IsValid())
			{
				return;
			}
			TestTrue(TEXT("stub_errors fingerprint"), FingerprintObj->Get()->HasField(TEXT("stub_errors")));

			// Stored entry is retrievable via the store.
			const FUnrealOpenMcpCheckpointStoreEntry* Stored = FUnrealOpenMcpCheckpointStore::Get(CheckpointId);
			TestNotNull(TEXT("stored entry"), Stored);
			if (Stored != nullptr)
			{
				TestEqual(TEXT("label mirrored"), Stored->Label, FString(TEXT("before-mutate")));
			}
		});
	});

	Describe("unreal_open_mcp_delta — handler contract", [this]()
	{
		It("checkpoint → mutate-simulate → delta reports new issues", [this]()
		{
			// Workflow: checkpoint against a single stub rule, then re-run
			// delta after registering a SECOND rule that emits a new issue.
			// The delta should report one new issue (the second rule's
			// finding) and zero resolved.
			FScopedState Guard;
			FVerifyRunner::RegisterRule(MakeUnique<FErrorRule>(TEXT("stub_errors")));

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpGateMetaTools::Register(Registry);

			// Step 1: capture the baseline.
			FUnrealOpenMcpToolHandler CheckpointHandler;
			Registry.TryGet(TEXT("unreal_open_mcp_checkpoint_create"), CheckpointHandler);
			const FString CbBody = FString::Printf(TEXT("{\"paths\":[\"%s\"]}"), FixturePath);
			const FUnrealOpenMcpToolDispatchResult CbResult = CheckpointHandler(CbBody);
			TestTrue(TEXT("checkpoint ok"), CbResult.bOk);
			const TSharedPtr<FJsonObject> CbJson = ParseJson(CbResult.Output);
			const FString CheckpointId = CbJson.IsValid() ? CbJson->GetStringField(TEXT("checkpointId")) : FString();
			TestTrue(TEXT("checkpointId set"), !CheckpointId.IsEmpty());

			// Step 2: simulate a mutation that introduces a new warning rule.
			FVerifyRunner::RegisterRule(MakeUnique<FWarningRule>(TEXT("stub_warnings")));

			// Step 3: delta against the baseline.
			FUnrealOpenMcpToolHandler DeltaHandler;
			Registry.TryGet(TEXT("unreal_open_mcp_delta"), DeltaHandler);
			const FString DeltaBody = FString::Printf(
				TEXT("{\"checkpoint_id\":\"%s\"}"), *CheckpointId);
			const FUnrealOpenMcpToolDispatchResult DeltaResult = DeltaHandler(DeltaBody);
			TestTrue(TEXT("delta ok"), DeltaResult.bOk);

			const TSharedPtr<FJsonObject> DeltaJson = ParseJson(DeltaResult.Output);
			if (!TestNotNull(TEXT("delta result json"), DeltaJson.Get()))
			{
				return;
			}
			// newWarnings = 1 (the stub_warnings rule's finding is new).
			const TSharedPtr<FJsonObject>* Summary = nullptr;
			TestTrue(TEXT("summary field"), DeltaJson->TryGetObjectField(TEXT("summary"), Summary));
			if (Summary == nullptr || !Summary->IsValid())
			{
				return;
			}
			TestEqual(TEXT("newErrors 0"), (*Summary)->GetNumberField(TEXT("newErrors")), 0.0);
			TestEqual(TEXT("newWarnings 1"), (*Summary)->GetNumberField(TEXT("newWarnings")), 1.0);
			TestEqual(TEXT("resolvedErrors 0"), (*Summary)->GetNumberField(TEXT("resolvedErrors")), 0.0);
			// `passed` is true because newWarnings alone do not fail the delta
			// (Errors take priority; warnings are informational).
			TestEqual(TEXT("passed true"), DeltaJson->GetBoolField(TEXT("passed")), true);
		});

		It("surfaces checkpointLostOnReload when the store is empty", [this]()
		{
			// Empty store + a specific id request → lost-on-reload payload
			// (the store tracks no process-lifetime marker, so the wording
			// covers both "wiped by reload" and "never created this session").
			FScopedState Guard; // clears the store

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpGateMetaTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_delta"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(
				TEXT("{\"checkpoint_id\":\"cp_deadbeef\"}"));
			TestTrue(TEXT("ok (not a hard error)"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("unavailable true"), Json->GetBoolField(TEXT("unavailable")), true);
			TestEqual(TEXT("checkpointLostOnReload true"), Json->GetBoolField(TEXT("checkpointLostOnReload")), true);
			TestEqual(TEXT("passed true"), Json->GetBoolField(TEXT("passed")), true);
			TestTrue(TEXT("warning set"), !Json->GetStringField(TEXT("warning")).IsEmpty());
			TestTrue(TEXT("agentNextSteps set"), Json->GetArrayField(TEXT("agentNextSteps")).Num() > 0);
		});

		It("surfaces unavailable (not lost-on-reload) when the store has other checkpoints", [this]()
		{
			// Store has at least one checkpoint, but the requested id is
			// unknown → unavailable payload WITHOUT checkpointLostOnReload
			// (the agent can distinguish "wiped by reload" from "this id was
			// never created in this session").
			FScopedState Guard;
			FVerifyRunner::RegisterRule(MakeUnique<FErrorRule>(TEXT("stub_errors")));

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpGateMetaTools::Register(Registry);
			FUnrealOpenMcpToolHandler CheckpointHandler;
			Registry.TryGet(TEXT("unreal_open_mcp_checkpoint_create"), CheckpointHandler);
			// Create a checkpoint so the store is non-empty.
			const FString CbBody = FString::Printf(TEXT("{\"paths\":[\"%s\"]}"), FixturePath);
			const FUnrealOpenMcpToolDispatchResult CbResult = CheckpointHandler(CbBody);
			TestTrue(TEXT("checkpoint ok"), CbResult.bOk);

			FUnrealOpenMcpToolHandler DeltaHandler;
			Registry.TryGet(TEXT("unreal_open_mcp_delta"), DeltaHandler);
			const FUnrealOpenMcpToolDispatchResult Result = DeltaHandler(
				TEXT("{\"checkpoint_id\":\"cp_unknown\"}"));
			TestTrue(TEXT("ok (not a hard error)"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("unavailable true"), Json->GetBoolField(TEXT("unavailable")), true);
			// checkpointLostOnReload is ABSENT in this branch (only set when
			// the store is empty). HasField returns false when the key is
			// absent.
			TestFalse(TEXT("checkpointLostOnReload absent"), Json->HasField(TEXT("checkpointLostOnReload")));
		});

		It("fails fast on missing_parameter when checkpoint_id is absent", [this]()
		{
			FScopedState Guard;
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpGateMetaTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_delta"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});
	});

	Describe("meta-tools are classified read-only (no gate recursion)", [this]()
	{
		It("all three register as non-mutating with gate Off", [this]()
		{
			// Pinned acceptance criterion: the meta-tools PARTICIPATE in the
			// gate workflow but must not recurse through GatePolicy.Execute.
			// The dispatch policy reads metadata to make that decision, so
			// the metadata must be the read-only default.
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpGateMetaTools::Register(Registry);

			for (const FString& Name : {
				FString(TEXT("unreal_open_mcp_validate_edit")),
				FString(TEXT("unreal_open_mcp_checkpoint_create")),
				FString(TEXT("unreal_open_mcp_delta")) })
			{
				FUnrealOpenMcpToolMetadata Metadata;
				TestTrue(*FString::Printf(TEXT("%s registered"), *Name), Registry.TryGetMetadata(Name, Metadata));
				TestFalse(*FString::Printf(TEXT("%s not mutating"), *Name), Metadata.bIsMutating);
				TestEqual(*FString::Printf(TEXT("%s gate Off"), *Name), Metadata.DefaultGate, EUnrealOpenMcpGateMode::Off);
			}
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
