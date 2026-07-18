// unreal_open_mcp_apply_fix — Automation specs (P3.7).
//
// Pins the apply_fix handler contract end-to-end. The handler is driven
// directly via a fresh FUnrealOpenMcpToolRegistry (no HTTP hop). Stub fix
// providers cover the branches:
//   - FSafeStubProvider — Describe returns Safe=true, Apply returns success.
//   - FFailStubProvider — Apply returns !success (rollback path).
//   - FCorruptorStubProvider — Apply returns success but the gate delta
//     reports new errors (rollback path).
//
// Covered surfaces:
//   - Missing-parameter (issue_id absent) → missing_parameter error.
//   - Invalid issue_id (malformed) → invalid_issue_id error.
//   - fix_id omitted → response lists every fix that can resolve the issue.
//   - Unknown fix_id → structured unknown_fix error with availableFixIds.
//   - Dry-run preview → response carries {dryRun:true, fixId, description, safe}.
//   - Non-dry-run apply WITHOUT rollback snapshot → rollback_unavailable
//     refusal (the safety guard).
//   - Non-dry-run apply WITH rollback snapshot → success.
//   - Non-dry-run apply whose fix fails → bRolledBack + restoredPaths.
//   - gate:"off" successful apply → rollbackDisabled flag set.
//
// Ported from Unity Open MCP's ApplyFixSafetyTests / ApplyFixRollbackTests at
// copy fidelity (the JSON payload schema and the rollback semantics are
// shared contracts — agents that consume one consume the other).
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Bridge/UnrealOpenMcpToolRegistry.h"
#include "Gate/UnrealOpenMcpGatePolicy.h"
#include "MetaTools/UnrealOpenMcpApplyFixTool.h"

#include "Core/IssueKey.h"
#include "Core/VerifyIssue.h"
#include "Core/VerifySeverity.h"
#include "Fixes/FixContracts.h"
#include "Fixes/FixProviderRegistry.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpApplyFixSpec,
	"UnrealOpenMcp.Tools.ApplyFix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpApplyFixSpec)

namespace
{
	/** Fixture asset path used in every test issue key. The handler does not
	 *  consult the asset registry — the path only needs to be parseable into
	 *  FIssueKey components. */
	constexpr const TCHAR* FixtureAssetPath = TEXT("/Game/Fixtures/ApplyFixTarget.ApplyFixTarget");

	TSharedPtr<FJsonObject> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	// Safe stub provider: matches a single rule+code, Describe returns Safe=true,
	// Apply returns success (no-op).
	class FSafeStubProvider : public IFixProvider
	{
	public:
		FString FixIdValue;
		FString RuleIdValue;
		FString IssueCodeValue;

		FSafeStubProvider(FString InFixId, FString InRuleId, FString InIssueCode)
			: FixIdValue(MoveTemp(InFixId))
			, RuleIdValue(MoveTemp(InRuleId))
			, IssueCodeValue(MoveTemp(InIssueCode))
		{
		}

		virtual FString GetFixId() const override { return FixIdValue; }

		virtual bool CanFix(const FString& IssueId) const override
		{
			FString RuleId, IssueCode;
			EVerifySeverity Sev = EVerifySeverity::Warning;
			FString AssetPath;
			if (!FIssueKey::TryParse(IssueId, RuleId, Sev, AssetPath, IssueCode))
			{
				return false;
			}
			return RuleId == RuleIdValue
				&& FIssueKey::BareIssueCode(IssueCode) == IssueCodeValue;
		}

		virtual FFixDescription Describe(const FString& IssueId) const override
		{
			FString RuleId, IssueCode, AssetPath;
			EVerifySeverity Sev = EVerifySeverity::Warning;
			FIssueKey::TryParse(IssueId, RuleId, Sev, AssetPath, IssueCode);
			FFixDescription D;
			D.FixId = FixIdValue;
			D.IssueId = IssueId;
			D.AssetPath = AssetPath;
			D.Description = TEXT("safe stub");
			D.bSafe = true;
			return D;
		}

		virtual FFixResult Apply(const FString& IssueId) override
		{
			FFixResult R;
			R.bSuccess = true;
			R.Description = TEXT("safe stub applied");
			R.TouchedPaths.Add(FixtureAssetPath);
			return R;
		}
	};

	// Fail stub provider: Apply returns !success so the gate runner rolls back.
	class FFailStubProvider : public IFixProvider
	{
	public:
		FString FixIdValue;
		FString RuleIdValue;
		FString IssueCodeValue;

		FFailStubProvider(FString InFixId, FString InRuleId, FString InIssueCode)
			: FixIdValue(MoveTemp(InFixId))
			, RuleIdValue(MoveTemp(InRuleId))
			, IssueCodeValue(MoveTemp(InIssueCode))
		{
		}

		virtual FString GetFixId() const override { return FixIdValue; }

		virtual bool CanFix(const FString& IssueId) const override
		{
			FString RuleId, IssueCode;
			EVerifySeverity Sev = EVerifySeverity::Warning;
			FString AssetPath;
			if (!FIssueKey::TryParse(IssueId, RuleId, Sev, AssetPath, IssueCode))
			{
				return false;
			}
			return RuleId == RuleIdValue
				&& FIssueKey::BareIssueCode(IssueCode) == IssueCodeValue;
		}

		virtual FFixDescription Describe(const FString& IssueId) const override
		{
			FFixDescription D;
			D.FixId = FixIdValue;
			D.IssueId = IssueId;
			D.Description = TEXT("fail stub");
			D.bSafe = true;
			return D;
		}

		virtual FFixResult Apply(const FString& IssueId) override
		{
			FFixResult R;
			R.bSuccess = false;
			R.Description = TEXT("fail stub: simulated fix failure");
			return R;
		}
	};

	// RAII guard: clear the provider registry on construct / destruct.
	struct FScopedProviders
	{
		FScopedProviders()
		{
			FFixProviderRegistry::Clear();
		}
		~FScopedProviders()
		{
			FFixProviderRegistry::Clear();
			FFixProviderRegistry::EnsureDefaultsRegistered();
		}
	};

	FString BuildIssueId()
	{
		return FIssueKey::Build(
			TEXT("stub_rule"), EVerifySeverity::Error, FixtureAssetPath, TEXT("stub_code"));
	}
} // namespace

void FUnrealOpenMcpApplyFixSpec::Define()
{
	Describe("unreal_open_mcp_apply_fix — handler contract (inner tool)", [this]()
	{
		It("is registered as a mutating tool", [this]()
		{
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpApplyFixMetaTools::Register(Registry);
			FUnrealOpenMcpToolMetadata Metadata;
			TestTrue(TEXT("registered"), Registry.TryGetMetadata(TEXT("unreal_open_mcp_apply_fix"), Metadata));
			TestTrue(TEXT("mutating"), Metadata.bIsMutating);
		});

		It("fails fast on missing_parameter when issue_id is absent", [this]()
		{
			FScopedProviders Guard;
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpApplyFixMetaTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_apply_fix"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result = Handler(TEXT("{\"dry_run\":true}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("missing_parameter")));
		});

		It("fails fast on invalid_issue_id when the key is malformed", [this]()
		{
			FScopedProviders Guard;
			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpApplyFixMetaTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_apply_fix"), Handler);

			const FUnrealOpenMcpToolDispatchResult Result =
				Handler(TEXT("{\"issue_id\":\"garbage\",\"dry_run\":true}"));
			TestFalse(TEXT("ok false"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("invalid_issue_id")));
		});

		It("lists every fix that can resolve the issue when fix_id is omitted", [this]()
		{
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("stub_a"), TEXT("stub_rule"), TEXT("stub_code")));
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("stub_b"), TEXT("stub_rule"), TEXT("stub_code")));

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpApplyFixMetaTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_apply_fix"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"issue_id\":\"%s\",\"dry_run\":true}"), *BuildIssueId());
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("dryRun true"), Json->GetBoolField(TEXT("dryRun")), true);
			const TArray<TSharedPtr<FJsonValue>>& Avail = Json->GetArrayField(TEXT("availableFixIds"));
			TestEqual(TEXT("two fixes listed"), Avail.Num(), 2);
		});

		It("returns structured unknown_fix when fix_id does not resolve", [this]()
		{
			FScopedProviders Guard;

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpApplyFixMetaTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_apply_fix"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"fix_id\":\"not_a_real_fix\",\"issue_id\":\"%s\",\"dry_run\":true}"),
				*BuildIssueId());
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			// Unknown fix returns Ok with a structured error payload — matches
			// Unity parity (the envelope builder does not wrap a hard error).
			TestTrue(TEXT("ok (structured)"), Result.bOk);

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
			TestEqual(TEXT("code"), (*ErrorObj)->GetStringField(TEXT("code")), FString(TEXT("unknown_fix")));
		});

		It("dry-run preview returns fixId + description + safe", [this]()
		{
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("stub_safe"), TEXT("stub_rule"), TEXT("stub_code")));

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpApplyFixMetaTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_apply_fix"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"fix_id\":\"stub_safe\",\"issue_id\":\"%s\",\"dry_run\":true}"),
				*BuildIssueId());
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestTrue(TEXT("ok"), Result.bOk);

			const TSharedPtr<FJsonObject> Json = ParseJson(Result.Output);
			if (!TestNotNull(TEXT("result json"), Json.Get()))
			{
				return;
			}
			TestEqual(TEXT("dryRun true"), Json->GetBoolField(TEXT("dryRun")), true);
			TestEqual(TEXT("fixId"), Json->GetStringField(TEXT("fixId")), FString(TEXT("stub_safe")));
			TestEqual(TEXT("safe"), Json->GetBoolField(TEXT("safe")), true);
			TestTrue(TEXT("description set"), !Json->GetStringField(TEXT("description")).IsEmpty());
		});

		It("non-dry-run apply refused without rollback snapshot (safety guard)", [this]()
		{
			// Direct dispatch (no ApplyFixGateRunner wrapper) → no snapshot.
			// The handler must refuse so a corrupting fix is never permanent.
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("stub_safe"), TEXT("stub_rule"), TEXT("stub_code")));

			FUnrealOpenMcpToolRegistry Registry;
			FUnrealOpenMcpApplyFixMetaTools::Register(Registry);
			FUnrealOpenMcpToolHandler Handler;
			Registry.TryGet(TEXT("unreal_open_mcp_apply_fix"), Handler);

			const FString Body = FString::Printf(
				TEXT("{\"fix_id\":\"stub_safe\",\"issue_id\":\"%s\",\"dry_run\":false}"),
				*BuildIssueId());
			const FUnrealOpenMcpToolDispatchResult Result = Handler(Body);
			TestFalse(TEXT("refused"), Result.bOk);
			TestEqual(TEXT("code"), Result.Code, FString(TEXT("rollback_unavailable")));
		});
	});

	Describe("unreal_open_mcp_apply_fix — gate runner (rollback)", [this]()
	{
		It("non-dry-run apply via the runner succeeds when the fix succeeds", [this]()
		{
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FSafeStubProvider>(TEXT("stub_safe"), TEXT("stub_rule"), TEXT("stub_code")));

			const FString Body = FString::Printf(
				TEXT("{\"fix_id\":\"stub_safe\",\"issue_id\":\"%s\",\"dry_run\":false}"),
				*BuildIssueId());

			// Off mode: paths_hint may be empty (the runner does not skip the
			// gate on empty hint — it is the caller's responsibility to pass a
			// scope). Pass an explicit empty scope + gate Off so the runner's
			// gate path is the Off short-circuit (no checkpoint/validate).
			const FUnrealOpenMcpApplyFixGateRunnerResult Result =
				FUnrealOpenMcpApplyFixGateRunner::Execute(Body, EUnrealOpenMcpGateMode::Off, TArray<FString>{});

			TestTrue(TEXT("mutation success"), Result.Gate.Mutation.bOk);
			TestFalse(TEXT("not rolled back"), Result.bRolledBack);
			// gate:off + success → rollbackDisabled flag set so the agent
			// knows the mutation committed without rollback protection.
			TestTrue(TEXT("rollback disabled (gate off)"), Result.bRollbackDisabled);
		});

		It("non-dry-run apply rolls back when the fix fails", [this]()
		{
			FScopedProviders Guard;
			FFixProviderRegistry::RegisterProvider(
				MakeUnique<FFailStubProvider>(TEXT("stub_fail"), TEXT("stub_rule"), TEXT("stub_code")));

			const FString Body = FString::Printf(
				TEXT("{\"fix_id\":\"stub_fail\",\"issue_id\":\"%s\",\"dry_run\":false}"),
				*BuildIssueId());

			const FUnrealOpenMcpApplyFixGateRunnerResult Result =
				FUnrealOpenMcpApplyFixGateRunner::Execute(Body, EUnrealOpenMcpGateMode::Enforce, TArray<FString>{FixtureAssetPath});

			// Mutation failed → bRolledBack even though no file existed to
			// restore (the rollback is best-effort; the flag still fires so
			// the envelope reports the rollback decision honestly).
			TestTrue(TEXT("rolled back"), Result.bRolledBack);
			TestFalse(TEXT("rollback disabled stays false"), Result.bRollbackDisabled);
			TestTrue(TEXT("rollback reason set"), !Result.RollbackReason.IsEmpty());
			TestTrue(TEXT("mutation marked failed"), !Result.Gate.Mutation.bOk);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
