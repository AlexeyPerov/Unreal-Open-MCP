// Project settings reader. See header for the schema + load semantics.
#include "Bridge/UnrealOpenMcpBridgeProjectSettings.h"

#include "Bridge/UnrealOpenMcpBridgeAuthPolicy.h"
#include "Bridge/UnrealOpenMcpBridgeBindAddress.h"
#include "Bridge/UnrealOpenMcpInstancePortResolver.h"
#include "UnrealOpenMcpLog.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

FString FUnrealOpenMcpBridgeProjectSettings::ResolveSettingsPath(const FString& ProjectRoot)
{
	// <projectRoot>/.unreal-open-mcp/settings.json
	return FPaths::Combine(ProjectRoot, FUnrealOpenMcpInstancePortResolver::SettingsDirName, FileName());
}

void FUnrealOpenMcpBridgeProjectSettings::BindToProject(const FString& ProjectRoot)
{
	ProjectRootPath = ProjectRoot;
	bLoaded = false;
	AuthMode = FString(FUnrealOpenMcpBridgeAuthPolicy::Default());
	BindAddress = FString(FUnrealOpenMcpBridgeBindAddress::Default());
}

void FUnrealOpenMcpBridgeProjectSettings::EnsureLoaded()
{
	if (bLoaded)
	{
		return;
	}
	bLoaded = true;
	AuthMode = FString(FUnrealOpenMcpBridgeAuthPolicy::Default());
	BindAddress = FString(FUnrealOpenMcpBridgeBindAddress::Default());

	if (ProjectRootPath.IsEmpty())
	{
		return; // no project bound → defaults
	}

	const FString Path = ResolveSettingsPath(ProjectRootPath);
	if (!IFileManager::Get().FileExists(*Path))
	{
		return; // missing file → defaults (silent)
	}

	FString JsonBody;
	if (!FFileHelper::LoadFileToString(JsonBody, *Path))
	{
		UE_LOG(
			LogUnrealOpenMcp,
			Warning,
			TEXT("[Unreal Open MCP] settings.json unreadable at %s — using defaults."),
			*Path);
		return;
	}

	ApplyJson(JsonBody);
}

void FUnrealOpenMcpBridgeProjectSettings::ApplyJson(const FString& JsonBody)
{
	const FString Path = ResolveSettingsPath(ProjectRootPath);

	TSharedPtr<FJsonObject> Object;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonBody);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		UE_LOG(
			LogUnrealOpenMcp,
			Warning,
			TEXT("[Unreal Open MCP] settings.json unparseable at %s — using defaults."),
			*Path);
		return;
	}

	// authMode: canonicalize through IsValid. An invalid value (typo, garbage)
	// coerces to the default "none" rather than failing closed — the project
	// default applies to benign localhost traffic. (The HTTP auth check itself
	// still fails closed on an unknown mode if a future code path bypasses this
	// canonicalization.)
	if (Object->HasTypedField<EJson::String>(TEXT("authMode")))
	{
		const FString Raw = Object->GetStringField(TEXT("authMode"));
		AuthMode = FUnrealOpenMcpBridgeAuthPolicy::IsValid(Raw)
			? Raw
			: FString(FUnrealOpenMcpBridgeAuthPolicy::Default());
	}

	// bindAddress: same coerce-to-safe-default pattern. Invalid values fall
	// back to loopback (never remote).
	if (Object->HasTypedField<EJson::String>(TEXT("bindAddress")))
	{
		const FString Raw = Object->GetStringField(TEXT("bindAddress"));
		BindAddress = FUnrealOpenMcpBridgeBindAddress::IsValid(Raw)
			? Raw
			: FString(FUnrealOpenMcpBridgeBindAddress::Default());
	}
}

FString FUnrealOpenMcpBridgeProjectSettings::GetAuthMode()
{
	EnsureLoaded();
	return AuthMode;
}

FString FUnrealOpenMcpBridgeProjectSettings::GetBindAddress()
{
	EnsureLoaded();
	return BindAddress;
}
