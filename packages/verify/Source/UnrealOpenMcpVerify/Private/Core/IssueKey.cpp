// FIssueKey implementation. See IssueKey.h for the format and the
// case-insensitive severity matcher rationale.
#include "Core/IssueKey.h"

namespace UnrealOpenMcpVerify
{

// Case-insensitive severity matcher. Recognizes the short ("WARN") and long
// ("WARNING") warning spellings plus "ERROR" so all producers (Build,
// scan_paths / validate_edit severity strings, hand-transcribed keys) parse
// uniformly. Returns false for anything else so a genuinely malformed
// severity (e.g. "CRITICAL") still rejects.
bool TryMatchSeverity(const FString& SevStr, EVerifySeverity& OutSeverity)
{
	OutSeverity = EVerifySeverity::Warning;
	if (SevStr.IsEmpty())
	{
		return false;
	}
	const FString Upper = SevStr.ToUpper();
	if (Upper == TEXT("ERROR"))
	{
		OutSeverity = EVerifySeverity::Error;
		return true;
	}
	if (Upper == TEXT("WARN") || Upper == TEXT("WARNING"))
	{
		OutSeverity = EVerifySeverity::Warning;
		return true;
	}
	return false;
}

bool ContainsPipe(const FString& S)
{
	return S.Contains(TEXT('|'));
}

} // namespace UnrealOpenMcpVerify

const TCHAR* FIssueKey::SeverityToken(const EVerifySeverity Severity)
{
	return Severity == EVerifySeverity::Error ? TEXT("ERROR") : TEXT("WARN");
}

FString FIssueKey::Build(
	const FString& RuleId,
	const EVerifySeverity Severity,
	const FString& AssetPath,
	const FString& IssueCode)
{
	using namespace UnrealOpenMcpVerify;

	// Component validation. The Unity version throws ArgumentException; here
	// we surface an empty key so a misuse is observable without crashing the
	// editor. The IssueKey spec pins every branch.
	if (RuleId.IsEmpty() || AssetPath.IsEmpty() || IssueCode.IsEmpty())
	{
		return FString();
	}
	if (ContainsPipe(RuleId) || ContainsPipe(AssetPath) || ContainsPipe(IssueCode))
	{
		return FString();
	}

	return FString::Printf(
		TEXT("%s|%s|%s|%s"),
		*RuleId,
		SeverityToken(Severity),
		*AssetPath,
		*IssueCode);
}

bool FIssueKey::TryParse(
	const FString& InKey,
	FString& OutRuleId,
	EVerifySeverity& OutSeverity,
	FString& OutAssetPath,
	FString& OutIssueCode)
{
	using namespace UnrealOpenMcpVerify;

	OutRuleId.Reset();
	OutAssetPath.Reset();
	OutIssueCode.Reset();
	OutSeverity = EVerifySeverity::Warning;

	if (InKey.IsEmpty())
	{
		return false;
	}

	// Split into exactly 4 fields. Mirrors Unity's key.Split('|') which does
	// NOT cull empties — a key like "rule|ERROR||code" yields 4 parts with an
	// empty AssetPath and is rejected by the emptiness check below (not the
	// count check). The count check rejects 3-part / 5-part inputs outright.
	TArray<FString> Parts;
	InKey.ParseIntoArray(Parts, TEXT("|"), /*bCullEmpty=*/false);
	if (Parts.Num() != 4)
	{
		return false;
	}

	const FString& SevStr = Parts[1];
	if (!TryMatchSeverity(SevStr, OutSeverity))
	{
		return false;
	}

	if (Parts[0].IsEmpty() || Parts[2].IsEmpty() || Parts[3].IsEmpty())
	{
		return false;
	}

	OutRuleId = Parts[0];
	OutAssetPath = Parts[2];
	OutIssueCode = Parts[3];
	return true;
}

FString FIssueKey::BareIssueCode(const FString& IssueCode)
{
	if (IssueCode.IsEmpty())
	{
		return IssueCode;
	}
	const int32 Colon = IssueCode.Find(TEXT(":"));
	if (Colon < 0)
	{
		return IssueCode;
	}
	return IssueCode.Left(Colon);
}

FString FIssueKey::IssueCodeSuffix(const FString& IssueCode)
{
	if (IssueCode.IsEmpty())
	{
		return FString();
	}
	const int32 Colon = IssueCode.Find(TEXT(":"));
	if (Colon < 0 || Colon + 1 >= IssueCode.Len())
	{
		return FString();
	}
	return IssueCode.Mid(Colon + 1);
}
