// Tool registry — see header for the Ok/Fail + Register/Contains/TryGet
// contract and the attribute-vs-manual discovery rationale.
#include "Bridge/UnrealOpenMcpToolRegistry.h"

#include "UnrealOpenMcpLog.h"

bool FUnrealOpenMcpToolRegistry::Register(const FString& Name, FUnrealOpenMcpToolHandler Handler)
{
	FScopeLock ScopeLock(&Lock);
	// First registration wins. A second Register for the same name is a no-op
	// so boot-order churn (two modules registering the same tool) never
	// silently swaps the handler — the original stays put and the duplicate is
	// logged-and-ignored. Matches Unity's first-registered-wins behavior.
	if (Tools.Contains(Name))
	{
		UE_LOG(
			LogUnrealOpenMcp,
			Warning,
			TEXT("[Unreal Open MCP] tool registry: duplicate registration for '%s' ignored (first wins)"),
			*Name);
		return false;
	}
	Tools.Add(Name, MoveTemp(Handler));
	return true;
}

bool FUnrealOpenMcpToolRegistry::Contains(const FString& Name) const
{
	FScopeLock ScopeLock(&Lock);
	return Tools.Contains(Name);
}

bool FUnrealOpenMcpToolRegistry::TryGet(const FString& Name, FUnrealOpenMcpToolHandler& OutHandler) const
{
	FScopeLock ScopeLock(&Lock);
	const FUnrealOpenMcpToolHandler* Found = Tools.Find(Name);
	if (Found == nullptr)
	{
		return false;
	}
	// Copy the TFunction out from under the lock so the caller can invoke it
	// without holding the registry mutex. TFunction copy is cheap (shared
	// state under the hood).
	OutHandler = *Found;
	return true;
}

TArray<FString> FUnrealOpenMcpToolRegistry::AllNames() const
{
	FScopeLock ScopeLock(&Lock);
	TArray<FString> Names;
	Tools.GetKeys(Names);
	return Names;
}

int32 FUnrealOpenMcpToolRegistry::Num() const
{
	FScopeLock ScopeLock(&Lock);
	return Tools.Num();
}
