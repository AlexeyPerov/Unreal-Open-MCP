// Editor module for the Unreal Open MCP plugin.
//
// Adapts Unity Open MCP's bridge lifecycle ([InitializeOnLoad] static ctor +
// EditorApplication.quitting) to Unreal's IModuleInterface: StartupModule owns
// boot + diagnostics, ShutdownModule owns teardown. The startup log line is
// the bridge's diagnostic proof of life — when triaging a missing /ping, the
// absence of this line means the plugin never loaded.
//
// P1.1 scope: logging only. HTTP listener, game-thread dispatcher, instance
// lock, and tool dispatch land in later phases. Startup/shutdown are
// idempotent: ShutdownModule guards against double-teardown and repeated
// module reloads (Live Coding / hot reload) never crash.
#include "Modules/ModuleManager.h"

#include "UnrealOpenMcpLog.h"
#include "Bridge/UnrealOpenMcpBridgeSession.h"

class FUnrealOpenMcpEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Proof-of-life line, logged FIRST so a later-startup hiccup never
		// hides that the module itself loaded. Includes the bridge version so
		// a single Output Log line answers "is the plugin on, and which build?"
		UE_LOG(
			LogUnrealOpenMcp,
			Log,
			TEXT("[Unreal Open MCP] plugin loaded (bridge version %s)"),
			FUnrealOpenMcpBridgeSession::GetBridgeVersion()
		);
	}

	virtual void ShutdownModule() override
	{
		// Idempotent: module shutdown may run after a reload cycle or during
		// editor teardown. Keep this path side-effect-free.
		UE_LOG(LogUnrealOpenMcp, Log, TEXT("[Unreal Open MCP] plugin shutting down"));
	}
};

IMPLEMENT_MODULE(FUnrealOpenMcpEditorModule, UnrealOpenMcpEditor)
