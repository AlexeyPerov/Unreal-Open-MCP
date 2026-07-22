// Bridge auth Automation specs (P5.6).
//
// Ports Unity Open MCP's BridgeAuthTokenTests / BridgeAuthCheckTests /
// BridgeBindAddressTests
// (packages/bridge/Tests/Editor/Bridge/*) to Unreal. Pins the auth policy
// matrix, the token format, the constant-time compare, the Bearer extraction,
// and the bind-address decision — all pure (no I/O, no Unreal editor APIs) so
// they run without a bridge instance.
//
// Coverage mirrors the Unity acceptance criteria:
//   - Generate() produces a 64-char lowercase-hex token (256 bits).
//   - EqualsConstantTime is true only for exact equal-length matches.
//   - ExtractBearer handles scheme case, whitespace, empty token, non-Bearer.
//   - IsAuthorized matrix: none=allow; required=check; unknown=fail closed.
//   - BindAddress::Decide: loopback always; remote requires "required".
//   - ProjectSettings: missing/invalid values coerce to safe defaults.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#include "Bridge/UnrealOpenMcpBridgeAuthCheck.h"
#include "Bridge/UnrealOpenMcpBridgeAuthPolicy.h"
#include "Bridge/UnrealOpenMcpBridgeAuthToken.h"
#include "Bridge/UnrealOpenMcpBridgeBindAddress.h"
#include "Bridge/UnrealOpenMcpBridgeProjectSettings.h"

BEGIN_DEFINE_SPEC(
	FUnrealOpenMcpBridgeAuthSpec,
	"UnrealOpenMcp.Bridge.Auth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FUnrealOpenMcpBridgeAuthSpec)

namespace
{
bool IsLowerHex(const FString& S)
{
	for (int32 i = 0; i < S.Len(); ++i)
	{
		const TCHAR c = S[i];
		const bool bDigit = c >= TEXT('0') && c <= TEXT('9');
		const bool bLower = c >= TEXT('a') && c <= TEXT('f');
		if (!bDigit && !bLower)
		{
			return false;
		}
	}
	return true;
}

FString MakeTempProjectRoot()
{
	const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	return FPaths::Combine(
		FPaths::ConvertRelativePathToFull(FPaths::SystemTempDir()),
		TEXT("unreal-open-mcp-tests"),
		Guid);
}

void RemoveDirRecursive(const FString& Path)
{
	if (!Path.IsEmpty())
	{
		IFileManager::Get().DeleteDirectory(*Path, /*bTree*/ true, /*bRequireExists*/ false);
	}
}
} // end anonymous namespace

void FUnrealOpenMcpBridgeAuthSpec::Define()
{
	Describe("AuthToken.Generate", [this]()
	{
		It("produces a 64-char lowercase-hex token", [this]()
		{
			const FString Token = FUnrealOpenMcpBridgeAuthToken::Generate();
			TestEqual(TEXT("hex length is 64"), Token.Len(), FUnrealOpenMcpBridgeAuthToken::HexLength);
			TestTrue(TEXT("all lowercase hex"), IsLowerHex(Token));
		});

		It("is never empty", [this]()
		{
			for (int32 i = 0; i < 8; ++i)
			{
				TestFalse(
					FString::Printf(TEXT("generate #%d not empty"), i),
					FUnrealOpenMcpBridgeAuthToken::Generate().IsEmpty());
			}
		});

		It("varies across calls", [this]()
		{
			const FString A = FUnrealOpenMcpBridgeAuthToken::Generate();
			const FString B = FUnrealOpenMcpBridgeAuthToken::Generate();
			TestNotEqual(TEXT("two consecutive mints differ"), A, B);
		});
	});

	Describe("AuthToken.EqualsConstantTime", [this]()
	{
		It("returns true for identical strings", [this]()
		{
			const FString T = FUnrealOpenMcpBridgeAuthToken::Generate();
			TestTrue(TEXT("self equals self"), FUnrealOpenMcpBridgeAuthToken::EqualsConstantTime(T, T));
		});

		It("returns false for different strings of the same length", [this]()
		{
			const FString T = FUnrealOpenMcpBridgeAuthToken::Generate();
			FString Wrong = T;
			// Flip the last hex char to a different valid hex char.
			const TCHAR Last = Wrong[Wrong.Len() - 1];
			Wrong[Wrong.Len() - 1] = (Last == TEXT('a')) ? TEXT('b') : TEXT('a');
			TestFalse(TEXT("differ in last char"), FUnrealOpenMcpBridgeAuthToken::EqualsConstantTime(T, Wrong));
		});

		It("returns false for different lengths", [this]()
		{
			const FString T = FUnrealOpenMcpBridgeAuthToken::Generate();
			TestFalse(TEXT("prefix vs full"), FUnrealOpenMcpBridgeAuthToken::EqualsConstantTime(T, T.Left(32)));
			TestFalse(TEXT("full vs prefix"), FUnrealOpenMcpBridgeAuthToken::EqualsConstantTime(T.Left(32), T));
		});

		It("treats empty as equal to empty", [this]()
		{
			TestTrue(TEXT("empty == empty"), FUnrealOpenMcpBridgeAuthToken::EqualsConstantTime(FString(), FString()));
		});

		It("returns false when one side is empty and the other is not", [this]()
		{
			const FString T = FUnrealOpenMcpBridgeAuthToken::Generate();
			TestFalse(TEXT("token vs empty"), FUnrealOpenMcpBridgeAuthToken::EqualsConstantTime(T, FString()));
		});
	});

	Describe("AuthToken.ExtractBearer", [this]()
	{
		It("extracts the token from a well-formed Bearer header", [this]()
		{
			const FString Token = TEXT("abc123");
			TestEqual(
				TEXT("Bearer abc123 -> abc123"),
				FUnrealOpenMcpBridgeAuthToken::ExtractBearer(FString::Printf(TEXT("Bearer %s"), *Token)),
				Token);
		});

		It("is case-insensitive on the scheme", [this]()
		{
			TestEqual(
				TEXT("bearer lowercase"),
				FUnrealOpenMcpBridgeAuthToken::ExtractBearer(TEXT("bearer deadbeef")),
				FString(TEXT("deadbeef")));
			TestEqual(
				TEXT("BEARER uppercase"),
				FUnrealOpenMcpBridgeAuthToken::ExtractBearer(TEXT("BEARER deadbeef")),
				FString(TEXT("deadbeef")));
		});

		It("trims surrounding whitespace", [this]()
		{
			TestEqual(
				TEXT("leading/trailing whitespace trimmed"),
				FUnrealOpenMcpBridgeAuthToken::ExtractBearer(TEXT("   Bearer   tok   ")),
				FString(TEXT("tok")));
		});

		It("returns empty when the header is empty", [this]()
		{
			TestTrue(TEXT("empty header -> empty"), FUnrealOpenMcpBridgeAuthToken::ExtractBearer(FString()).IsEmpty());
			TestTrue(TEXT("whitespace-only -> empty"), FUnrealOpenMcpBridgeAuthToken::ExtractBearer(TEXT("   ")).IsEmpty());
		});

		It("returns empty when the scheme is not Bearer", [this]()
		{
			TestTrue(TEXT("Basic -> empty"), FUnrealOpenMcpBridgeAuthToken::ExtractBearer(TEXT("Basic abc123")).IsEmpty());
			TestTrue(TEXT("no scheme -> empty"), FUnrealOpenMcpBridgeAuthToken::ExtractBearer(TEXT("abc123")).IsEmpty());
		});

		It("returns empty for a bare Bearer prefix with no token", [this]()
		{
			TestTrue(TEXT("Bearer alone -> empty"), FUnrealOpenMcpBridgeAuthToken::ExtractBearer(TEXT("Bearer ")).IsEmpty());
			TestTrue(TEXT("Bearer (no space) -> empty"), FUnrealOpenMcpBridgeAuthToken::ExtractBearer(TEXT("Bearer")).IsEmpty());
		});
	});

	Describe("AuthPolicy", [this]()
	{
		It("accepts only none and required", [this]()
		{
			TestTrue(TEXT("none valid"), FUnrealOpenMcpBridgeAuthPolicy::IsValid(FUnrealOpenMcpBridgeAuthPolicy::None()));
			TestTrue(TEXT("required valid"), FUnrealOpenMcpBridgeAuthPolicy::IsValid(FUnrealOpenMcpBridgeAuthPolicy::Required()));
			TestFalse(TEXT("empty invalid"), FUnrealOpenMcpBridgeAuthPolicy::IsValid(FString()));
			TestFalse(TEXT("garbage invalid"), FUnrealOpenMcpBridgeAuthPolicy::IsValid(TEXT("yes")));
			TestFalse(TEXT("Required (case) invalid"), FUnrealOpenMcpBridgeAuthPolicy::IsValid(TEXT("Required")));
		});

		It("defaults to none", [this]()
		{
			TestEqual(
				TEXT("default == none"),
				FString(FUnrealOpenMcpBridgeAuthPolicy::Default()),
				FString(FUnrealOpenMcpBridgeAuthPolicy::None()));
		});
	});

	Describe("AuthCheck.IsAuthorized matrix", [this]()
	{
		const FString Token = FUnrealOpenMcpBridgeAuthToken::Generate();
		const FString GoodHeader = FString::Printf(TEXT("Bearer %s"), *Token);

		It("allows everything when policy is none", [this]()
		{
			TestTrue(TEXT("none + no header"), FUnrealOpenMcpBridgeAuthCheck::IsAuthorized(
				FUnrealOpenMcpBridgeAuthPolicy::None(), FString(), Token));
			TestTrue(TEXT("none + wrong header"), FUnrealOpenMcpBridgeAuthCheck::IsAuthorized(
				FUnrealOpenMcpBridgeAuthPolicy::None(), TEXT("Bearer wrong"), Token));
			TestTrue(TEXT("none + empty token"), FUnrealOpenMcpBridgeAuthCheck::IsAuthorized(
				FUnrealOpenMcpBridgeAuthPolicy::None(), FString(), FString()));
		});

		It("fails closed on an unknown policy", [this]()
		{
			TestFalse(TEXT("garbage + good header"), FUnrealOpenMcpBridgeAuthCheck::IsAuthorized(
				TEXT("yes"), GoodHeader, Token));
			TestFalse(TEXT("empty + good header"), FUnrealOpenMcpBridgeAuthCheck::IsAuthorized(
				FString(), GoodHeader, Token));
			TestFalse(TEXT("null + no header"), FUnrealOpenMcpBridgeAuthCheck::IsAuthorized(
				FString(), FString(), Token));
		});

		It("requires a non-empty expected token under required", [this]()
		{
			TestFalse(TEXT("required + empty token + header"), FUnrealOpenMcpBridgeAuthCheck::IsAuthorized(
				FUnrealOpenMcpBridgeAuthPolicy::Required(), GoodHeader, FString()));
			TestFalse(TEXT("required + empty token + no header"), FUnrealOpenMcpBridgeAuthCheck::IsAuthorized(
				FUnrealOpenMcpBridgeAuthPolicy::Required(), FString(), FString()));
		});

		It("rejects missing/malformed headers under required", [this]()
		{
			TestFalse(TEXT("required + no header"), FUnrealOpenMcpBridgeAuthCheck::IsAuthorized(
				FUnrealOpenMcpBridgeAuthPolicy::Required(), FString(), Token));
			TestFalse(TEXT("required + non-Bearer"), FUnrealOpenMcpBridgeAuthCheck::IsAuthorized(
				FUnrealOpenMcpBridgeAuthPolicy::Required(), TEXT("Basic abc"), Token));
			TestFalse(TEXT("required + bare Bearer"), FUnrealOpenMcpBridgeAuthCheck::IsAuthorized(
				FUnrealOpenMcpBridgeAuthPolicy::Required(), TEXT("Bearer "), Token));
		});

		It("rejects a wrong token under required", [this]()
		{
			TestFalse(TEXT("required + wrong token"), FUnrealOpenMcpBridgeAuthCheck::IsAuthorized(
				FUnrealOpenMcpBridgeAuthPolicy::Required(), TEXT("Bearer deadbeef"), Token));
		});

		It("accepts the correct token under required", [this]()
		{
			TestTrue(TEXT("required + correct token"), FUnrealOpenMcpBridgeAuthCheck::IsAuthorized(
				FUnrealOpenMcpBridgeAuthPolicy::Required(), GoodHeader, Token));
		});
	});

	Describe("BindAddress", [this]()
	{
		It("recognizes the two valid literals", [this]()
		{
			TestTrue(TEXT("loopback valid"), FUnrealOpenMcpBridgeBindAddress::IsValid(FUnrealOpenMcpBridgeBindAddress::Loopback()));
			TestTrue(TEXT("remote valid"), FUnrealOpenMcpBridgeBindAddress::IsValid(FUnrealOpenMcpBridgeBindAddress::Remote()));
			TestFalse(TEXT("garbage invalid"), FUnrealOpenMcpBridgeBindAddress::IsValid(TEXT("localhost")));
			TestFalse(TEXT("empty invalid"), FUnrealOpenMcpBridgeBindAddress::IsValid(FString()));
		});

		It("allows loopback regardless of auth mode", [this]()
		{
			const auto D1 = FUnrealOpenMcpBridgeBindAddress::Decide(
				FUnrealOpenMcpBridgeBindAddress::Loopback(), FUnrealOpenMcpBridgeAuthPolicy::None());
			TestTrue(TEXT("loopback + none allowed"), D1.bAllowed);
			TestEqual(TEXT("resolved loopback"), D1.ResolvedAddress, FString(FUnrealOpenMcpBridgeBindAddress::Loopback()));

			const auto D2 = FUnrealOpenMcpBridgeBindAddress::Decide(
				FUnrealOpenMcpBridgeBindAddress::Loopback(), FUnrealOpenMcpBridgeAuthPolicy::Required());
			TestTrue(TEXT("loopback + required allowed"), D2.bAllowed);
		});

		It("refuses remote unless authMode is required", [this]()
		{
			const auto D1 = FUnrealOpenMcpBridgeBindAddress::Decide(
				FUnrealOpenMcpBridgeBindAddress::Remote(), FUnrealOpenMcpBridgeAuthPolicy::None());
			TestFalse(TEXT("remote + none refused"), D1.bAllowed);
			TestTrue(TEXT("refusal reason set"), !D1.RefusalReason.IsEmpty());

			// Unknown auth mode also refuses (fail-closed).
			const auto D2 = FUnrealOpenMcpBridgeBindAddress::Decide(
				FUnrealOpenMcpBridgeBindAddress::Remote(), FString());
			TestFalse(TEXT("remote + unknown refused"), D2.bAllowed);
		});

		It("allows remote when authMode is required", [this]()
		{
			const auto D = FUnrealOpenMcpBridgeBindAddress::Decide(
				FUnrealOpenMcpBridgeBindAddress::Remote(), FUnrealOpenMcpBridgeAuthPolicy::Required());
			TestTrue(TEXT("remote + required allowed"), D.bAllowed);
			TestEqual(TEXT("resolved remote"), D.ResolvedAddress, FString(FUnrealOpenMcpBridgeBindAddress::Remote()));
		});

		It("coerces an invalid bind address to loopback", [this]()
		{
			const auto D = FUnrealOpenMcpBridgeBindAddress::Decide(
				TEXT("192.168.1.1"), FUnrealOpenMcpBridgeAuthPolicy::None());
			TestTrue(TEXT("garbage -> allowed (coerced)"), D.bAllowed);
			TestEqual(TEXT("resolved to loopback"), D.ResolvedAddress, FString(FUnrealOpenMcpBridgeBindAddress::Loopback()));
		});
	});

	Describe("ProjectSettings", [this]()
	{
		It("returns defaults when no settings file exists", [this]()
		{
			const FString Root = MakeTempProjectRoot();
			IFileManager::Get().MakeDirectory(*Root, /*bTree*/ true);

			FUnrealOpenMcpBridgeProjectSettings Settings;
			Settings.BindToProject(Root);
			TestEqual(TEXT("authMode default none"), Settings.GetAuthMode(), FString(FUnrealOpenMcpBridgeAuthPolicy::None()));
			TestEqual(TEXT("bindAddress default loopback"), Settings.GetBindAddress(), FString(FUnrealOpenMcpBridgeBindAddress::Loopback()));

			RemoveDirRecursive(Root);
		});

		It("reads authMode and bindAddress from a valid file", [this]()
		{
			const FString Root = MakeTempProjectRoot();
			const FString Dir = FPaths::Combine(Root, TEXT(".unreal-open-mcp"));
			IFileManager::Get().MakeDirectory(*Dir, /*bTree*/ true);
			const FString Path = FPaths::Combine(Dir, TEXT("settings.json"));
			FFileHelper::SaveStringToFile(TEXT("{\"authMode\":\"required\",\"bindAddress\":\"0.0.0.0\"}"), *Path);

			FUnrealOpenMcpBridgeProjectSettings Settings;
			Settings.BindToProject(Root);
			TestEqual(TEXT("authMode required"), Settings.GetAuthMode(), FString(FUnrealOpenMcpBridgeAuthPolicy::Required()));
			TestEqual(TEXT("bindAddress remote"), Settings.GetBindAddress(), FString(FUnrealOpenMcpBridgeBindAddress::Remote()));

			RemoveDirRecursive(Root);
		});

		It("coerces invalid values to safe defaults", [this]()
		{
			const FString Root = MakeTempProjectRoot();
			const FString Dir = FPaths::Combine(Root, TEXT(".unreal-open-mcp"));
			IFileManager::Get().MakeDirectory(*Dir, /*bTree*/ true);
			const FString Path = FPaths::Combine(Dir, TEXT("settings.json"));
			FFileHelper::SaveStringToFile(TEXT("{\"authMode\":\"bogus\",\"bindAddress\":\"999.999.999.999\"}"), *Path);

			FUnrealOpenMcpBridgeProjectSettings Settings;
			Settings.BindToProject(Root);
			TestEqual(TEXT("invalid authMode -> none"), Settings.GetAuthMode(), FString(FUnrealOpenMcpBridgeAuthPolicy::None()));
			TestEqual(TEXT("invalid bindAddress -> loopback"), Settings.GetBindAddress(), FString(FUnrealOpenMcpBridgeBindAddress::Loopback()));

			RemoveDirRecursive(Root);
		});

		It("coerces unparseable JSON to defaults", [this]()
		{
			const FString Root = MakeTempProjectRoot();
			const FString Dir = FPaths::Combine(Root, TEXT(".unreal-open-mcp"));
			IFileManager::Get().MakeDirectory(*Dir, /*bTree*/ true);
			const FString Path = FPaths::Combine(Dir, TEXT("settings.json"));
			FFileHelper::SaveStringToFile(TEXT("{not json"), *Path);

			FUnrealOpenMcpBridgeProjectSettings Settings;
			Settings.BindToProject(Root);
			TestEqual(TEXT("unparseable -> authMode none"), Settings.GetAuthMode(), FString(FUnrealOpenMcpBridgeAuthPolicy::None()));
			TestEqual(TEXT("unparseable -> bindAddress loopback"), Settings.GetBindAddress(), FString(FUnrealOpenMcpBridgeBindAddress::Loopback()));

			RemoveDirRecursive(Root);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
