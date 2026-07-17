// FIssueKey — deterministic identity for a VerifyIssue, used for gate delta
// diffs and the apply_fix lookup.
//
// Ported from Unity Open MCP packages/verify/Editor/Core/IssueKey.cs at copy
// fidelity. The format is `{RuleId}|{SEVERITY}|{AssetPath}|{IssueCode}`,
// where SEVERITY is the short token ERROR / WARN. The parser is
// case-insensitive on the severity token and accepts the long form WARNING
// too, so a hand-transcribed key (or one emitted by a different producer)
// still parses — the documented scan → apply_fix loop must work across
// separate calls.
//
// Some issue codes carry a GUID suffix (e.g. "missing_guid:<guid>") so the
// fix provider can identify exactly which broken reference to rewrite when
// an asset has multiple. The bare code (without the suffix) is what the
// explainability table and FixProviderRegistry key on; the helpers below
// strip / extract the suffix.
#pragma once

#include "CoreMinimal.h"

#include "Core/VerifyIssue.h"
#include "Core/VerifySeverity.h"

#include "IssueKey.generated.h"

USTRUCT(BlueprintType)
struct UNREALOPENMCPVERIFY_API FIssueKey
{
	GENERATED_BODY()

	// The canonical string form: {RuleId}|{SEVERITY}|{AssetPath}|{IssueCode}.
	// Empty when constructed from a malformed input (TryParse failed).
	UPROPERTY(VisibleAnywhere, Category = "UnrealOpenMcp|Verify")
	FString Key;

	FIssueKey() = default;

	explicit FIssueKey(FString InKey)
		: Key(MoveTemp(InKey))
	{
	}

	// True when Key is non-empty and well-formed.
	bool IsValid() const { return !Key.IsEmpty(); }

	// Build the canonical key from components. Asserts / returns empty on
	// invalid input (the Unity version throws; here we surface an empty key
	// so a misuse is observable without crashing the editor).
	static UNREALOPENMCPVERIFY_API FString Build(
		const FString& RuleId,
		EVerifySeverity Severity,
		const FString& AssetPath,
		const FString& IssueCode);

	// Build the canonical key from an issue.
	static FString Build(const FVerifyIssue& Issue)
	{
		return Build(Issue.RuleId, Issue.Severity, Issue.AssetPath, Issue.IssueCode);
	}

	// Parse the canonical key back into its components. Returns false on any
	// malformed input (wrong part count, unknown severity token, empty
	// required field). Case-insensitive on the severity token; accepts
	// ERROR / WARN / WARNING in any casing.
	static UNREALOPENMCPVERIFY_API bool TryParse(
		const FString& InKey,
		FString& OutRuleId,
		EVerifySeverity& OutSeverity,
		FString& OutAssetPath,
		FString& OutIssueCode);

	// Validate a key string (does not throw — returns false on malformed).
	static bool ValidateKey(const FString& InKey)
	{
		FString RuleId, AssetPath, IssueCode;
		EVerifySeverity Severity = EVerifySeverity::Warning;
		return TryParse(InKey, RuleId, Severity, AssetPath, IssueCode);
	}

	// Strip the ":<suffix>" tail from an issue code. Codes without a ":"
	// suffix are returned as-is (the common case).
	static UNREALOPENMCPVERIFY_API FString BareIssueCode(const FString& IssueCode);

	// Extract the suffix from an issue code like "missing_guid:<guid>".
	// Returns empty when the code has no suffix.
	static UNREALOPENMCPVERIFY_API FString IssueCodeSuffix(const FString& IssueCode);

	// Short severity token used in the canonical key: ERROR or WARN.
	static UNREALOPENMCPVERIFY_API const TCHAR* SeverityToken(EVerifySeverity Severity);
};
