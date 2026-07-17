// FIssueKey Automation specs (P3.1).
//
// Ports Unity Open MCP's IssueKeyTests
// (packages/verify/Tests/Editor/IssueKeyTests.cs) to Unreal at copy fidelity.
// Pins:
//   - canonical Build format (ERROR vs WARN token)
//   - 5-arg / 6-arg constructors do not affect the key
//   - empty / pipe-containing components return an empty key (Unity throws;
//     the Unreal port surfaces an empty key so a misuse is observable without
//     crashing the editor)
//   - TryParse accepts any case of ERROR / WARN / WARNING (regression guard
//     for the scan_paths → apply_fix loop pinned in specs/feedback.md
//     2026-07-03)
//   - BareIssueCode / IssueCodeSuffix strip and extract the GUID suffix
//   - Build ⇄ TryParse round-trip preserves every component
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Core/IssueKey.h"
#include "Core/VerifyIssue.h"
#include "Core/VerifySeverity.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpIssueKeySpec,
	"UnrealOpenMcp.Verify.IssueKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpIssueKeySpec)

void FUnrealOpenMcpIssueKeySpec::Define()
{
	Describe("Build", [this]()
	{
		It("produces the canonical format for an Error", [this]()
		{
			const FString Key = FIssueKey::Build(
				TEXT("missing_references"), EVerifySeverity::Error,
				TEXT("/Game/Fixtures/Test.uasset"), TEXT("missing_script"));

			TestEqual(TEXT("key"), Key,
				TEXT("missing_references|ERROR|/Game/Fixtures/Test.uasset|missing_script"));
		});

		It("produces the canonical format for a Warning", [this]()
		{
			const FString Key = FIssueKey::Build(
				TEXT("scene_health"), EVerifySeverity::Warning,
				TEXT("/Game/Maps/Main.umap"), TEXT("deep_nesting"));

			TestEqual(TEXT("key"), Key,
				TEXT("scene_health|WARN|/Game/Maps/Main.umap|deep_nesting"));
		});

		It("matches a direct Build when built from an issue", [this]()
		{
			const FVerifyIssue Issue(
				TEXT("missing_references"), EVerifySeverity::Error,
				TEXT("/Game/Fixtures/Prefab.uasset"), TEXT("missing_guid"),
				TEXT("desc"));

			TestEqual(
				TEXT("Build(issue) == Build(components)"),
				FIssueKey::Build(Issue),
				FIssueKey::Build(Issue.RuleId, Issue.Severity, Issue.AssetPath, Issue.IssueCode));
		});

		It("returns empty for an empty RuleId", [this]()
		{
			TestEqual(TEXT("empty key"),
				FIssueKey::Build(FString(), EVerifySeverity::Error, TEXT("/Game/A.uasset"), TEXT("code")),
				FString());
		});

		It("returns empty for an empty AssetPath", [this]()
		{
			TestEqual(TEXT("empty key"),
				FIssueKey::Build(TEXT("rule"), EVerifySeverity::Error, FString(), TEXT("code")),
				FString());
		});

		It("returns empty for an empty IssueCode", [this]()
		{
			TestEqual(TEXT("empty key"),
				FIssueKey::Build(TEXT("rule"), EVerifySeverity::Error, TEXT("/Game/A.uasset"), FString()),
				FString());
		});

		It("returns empty when RuleId contains a pipe", [this]()
		{
			TestEqual(TEXT("empty key"),
				FIssueKey::Build(TEXT("bad|rule"), EVerifySeverity::Error, TEXT("/Game/A.uasset"), TEXT("code")),
				FString());
		});

		It("returns empty when AssetPath contains a pipe", [this]()
		{
			TestEqual(TEXT("empty key"),
				FIssueKey::Build(TEXT("rule"), EVerifySeverity::Error, TEXT("bad|path"), TEXT("code")),
				FString());
		});

		It("returns empty when IssueCode contains a pipe", [this]()
		{
			TestEqual(TEXT("empty key"),
				FIssueKey::Build(TEXT("rule"), EVerifySeverity::Error, TEXT("/Game/A.uasset"), TEXT("bad|code")),
				FString());
		});
	});

	Describe("TryParse", [this]()
	{
		It("parses a valid Error key", [this]()
		{
			FString RuleId, AssetPath, IssueCode;
			EVerifySeverity Severity = EVerifySeverity::Warning;

			const bool bOk = FIssueKey::TryParse(
				TEXT("missing_references|ERROR|/Game/A.uasset|missing_script"),
				RuleId, Severity, AssetPath, IssueCode);

			TestTrue(TEXT("ok"), bOk);
			TestEqual(TEXT("ruleId"), RuleId, FString(TEXT("missing_references")));
			TestEqual(TEXT("severity"), Severity, EVerifySeverity::Error);
			TestEqual(TEXT("assetPath"), AssetPath, FString(TEXT("/Game/A.uasset")));
			TestEqual(TEXT("issueCode"), IssueCode, FString(TEXT("missing_script")));
		});

		It("parses a valid Warning key", [this]()
		{
			FString RuleId, AssetPath, IssueCode;
			EVerifySeverity Severity = EVerifySeverity::Error;

			const bool bOk = FIssueKey::TryParse(
				TEXT("scene_health|WARN|/Game/B.umap|deep_nesting"),
				RuleId, Severity, AssetPath, IssueCode);

			TestTrue(TEXT("ok"), bOk);
			TestEqual(TEXT("ruleId"), RuleId, FString(TEXT("scene_health")));
			TestEqual(TEXT("severity"), Severity, EVerifySeverity::Warning);
			TestEqual(TEXT("assetPath"), AssetPath, FString(TEXT("/Game/B.umap")));
			TestEqual(TEXT("issueCode"), IssueCode, FString(TEXT("deep_nesting")));
		});

		It("returns false for an empty key", [this]()
		{
			FString RuleId, AssetPath, IssueCode;
			EVerifySeverity Severity = EVerifySeverity::Warning;
			TestFalse(TEXT("ok"), FIssueKey::TryParse(FString(), RuleId, Severity, AssetPath, IssueCode));
		});

		It("returns false for the wrong part count", [this]()
		{
			FString RuleId, AssetPath, IssueCode;
			EVerifySeverity Severity = EVerifySeverity::Warning;
			TestFalse(TEXT("3 parts"), FIssueKey::TryParse(TEXT("a|b|c"), RuleId, Severity, AssetPath, IssueCode));
			TestFalse(TEXT("5 parts"), FIssueKey::TryParse(TEXT("a|b|c|d|e"), RuleId, Severity, AssetPath, IssueCode));
		});

		It("returns false for an invalid severity token", [this]()
		{
			FString RuleId, AssetPath, IssueCode;
			EVerifySeverity Severity = EVerifySeverity::Warning;
			TestFalse(TEXT("ok"),
				FIssueKey::TryParse(
					TEXT("rule|CRITICAL|/Game/A.uasset|code"),
					RuleId, Severity, AssetPath, IssueCode));
		});

		// Regression: specs/feedback.md 2026-07-03 — scan_paths / validate_edit
		// emit severity as "Error"/"Warning" (long form), but a parser that
		// only accepted "ERROR"/"WARN" broke the scan → apply_fix loop across
		// separate calls. TryParse must accept any case of ERROR / WARN /
		// WARNING so the loop works.
		It("accepts scan_paths severity casing (Error / Warning)", [this]()
		{
			FString RuleId, AssetPath, IssueCode;
			EVerifySeverity Severity = EVerifySeverity::Warning;

			TestTrue(TEXT("Error parses"),
				FIssueKey::TryParse(
					TEXT("missing_references|Error|/Game/A.uasset|missing_script"),
					RuleId, Severity, AssetPath, IssueCode));
			TestEqual(TEXT("severity"), Severity, EVerifySeverity::Error);

			TestTrue(TEXT("Warning parses"),
				FIssueKey::TryParse(
					TEXT("scene_health|Warning|/Game/B.umap|deep_nesting"),
					RuleId, Severity, AssetPath, IssueCode));
			TestEqual(TEXT("severity"), Severity, EVerifySeverity::Warning);
		});

		It("accepts the long-form WARNING spelling", [this]()
		{
			FString RuleId, AssetPath, IssueCode;
			EVerifySeverity Severity = EVerifySeverity::Error;
			TestTrue(TEXT("WARNING parses"),
				FIssueKey::TryParse(
					TEXT("rule|WARNING|/Game/A.uasset|code"),
					RuleId, Severity, AssetPath, IssueCode));
			TestEqual(TEXT("severity"), Severity, EVerifySeverity::Warning);
		});

		It("accepts lowercase severity tokens", [this]()
		{
			FString RuleId, AssetPath, IssueCode;
			EVerifySeverity Severity = EVerifySeverity::Warning;

			TestTrue(TEXT("error parses"),
				FIssueKey::TryParse(
					TEXT("rule|error|/Game/A.uasset|code"),
					RuleId, Severity, AssetPath, IssueCode));
			TestEqual(TEXT("severity"), Severity, EVerifySeverity::Error);

			TestTrue(TEXT("warn parses"),
				FIssueKey::TryParse(
					TEXT("rule|warn|/Game/A.uasset|code"),
					RuleId, Severity, AssetPath, IssueCode));
			TestEqual(TEXT("severity"), Severity, EVerifySeverity::Warning);
		});

		It("returns false for an empty RuleId component", [this]()
		{
			FString RuleId, AssetPath, IssueCode;
			EVerifySeverity Severity = EVerifySeverity::Warning;
			TestFalse(TEXT("ok"),
				FIssueKey::TryParse(TEXT("|ERROR|/Game/A.uasset|code"), RuleId, Severity, AssetPath, IssueCode));
		});

		It("returns false for an empty AssetPath component", [this]()
		{
			FString RuleId, AssetPath, IssueCode;
			EVerifySeverity Severity = EVerifySeverity::Warning;
			TestFalse(TEXT("ok"),
				FIssueKey::TryParse(TEXT("rule|ERROR||code"), RuleId, Severity, AssetPath, IssueCode));
		});

		It("returns false for an empty IssueCode component", [this]()
		{
			FString RuleId, AssetPath, IssueCode;
			EVerifySeverity Severity = EVerifySeverity::Warning;
			TestFalse(TEXT("ok"),
				FIssueKey::TryParse(TEXT("rule|ERROR|/Game/A.uasset|"), RuleId, Severity, AssetPath, IssueCode));
		});
	});

	Describe("ValidateKey", [this]()
	{
		It("returns true for a valid key", [this]()
		{
			TestTrue(TEXT("ok"),
				FIssueKey::ValidateKey(TEXT("missing_references|ERROR|/Game/A.uasset|missing_script")));
		});

		It("returns false for a malformed key", [this]()
		{
			TestFalse(TEXT("ok"), FIssueKey::ValidateKey(TEXT("bad-key")));
		});
	});

	Describe("Round-trip", [this]()
	{
		It("preserves every component through Build then TryParse", [this]()
		{
			const FString Original = FIssueKey::Build(
				TEXT("scene_health"), EVerifySeverity::Warning,
				TEXT("/Game/Maps/Game.umap"), TEXT("override_explosion"));

			FString RuleId, AssetPath, IssueCode;
			EVerifySeverity Severity = EVerifySeverity::Error;
			TestTrue(TEXT("parsed"), FIssueKey::TryParse(Original, RuleId, Severity, AssetPath, IssueCode));
			TestEqual(TEXT("ruleId"), RuleId, FString(TEXT("scene_health")));
			TestEqual(TEXT("severity"), Severity, EVerifySeverity::Warning);
			TestEqual(TEXT("assetPath"), AssetPath, FString(TEXT("/Game/Maps/Game.umap")));
			TestEqual(TEXT("issueCode"), IssueCode, FString(TEXT("override_explosion")));
		});
	});

	Describe("BareIssueCode / IssueCodeSuffix", [this]()
	{
		It("returns the code as-is when there is no suffix", [this]()
		{
			TestEqual(TEXT("bare"), FIssueKey::BareIssueCode(TEXT("missing_script")), FString(TEXT("missing_script")));
			TestEqual(TEXT("suffix empty"), FIssueKey::IssueCodeSuffix(TEXT("missing_script")), FString());
		});

		It("strips the GUID suffix from a missing_guid:<guid> code", [this]()
		{
			const FString Code = TEXT("missing_guid:1234567890abcdef1234567890abcdef");
			TestEqual(TEXT("bare"), FIssueKey::BareIssueCode(Code), FString(TEXT("missing_guid")));
			TestEqual(TEXT("suffix"),
				FIssueKey::IssueCodeSuffix(Code),
				FString(TEXT("1234567890abcdef1234567890abcdef")));
		});

		It("returns empty for a bare colon suffix", [this]()
		{
			TestEqual(TEXT("suffix empty when trailing colon"),
				FIssueKey::IssueCodeSuffix(TEXT("missing_guid:")),
				FString());
		});

		It("returns the input as-is for an empty code", [this]()
		{
			TestEqual(TEXT("bare empty"), FIssueKey::BareIssueCode(FString()), FString());
			TestEqual(TEXT("suffix empty"), FIssueKey::IssueCodeSuffix(FString()), FString());
		});
	});

	Describe("SeverityToken", [this]()
	{
		It("emits ERROR for Error severity", [this]()
		{
			TestEqual(TEXT("token"),
				FString(FIssueKey::SeverityToken(EVerifySeverity::Error)),
				FString(TEXT("ERROR")));
		});

		It("emits WARN for Warning severity", [this]()
		{
			TestEqual(TEXT("token"),
				FString(FIssueKey::SeverityToken(EVerifySeverity::Warning)),
				FString(TEXT("WARN")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
