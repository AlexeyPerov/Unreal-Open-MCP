// Editor module for the Unreal Open MCP plugin.
//
// Adapts Unity Open MCP's bridge lifecycle ([InitializeOnLoad] static ctor +
// EditorApplication.quitting) to Unreal's IModuleInterface: StartupModule owns
// boot + diagnostics, ShutdownModule owns teardown. The startup log line is
// the bridge's diagnostic proof of life — when triaging a missing /ping, the
// absence of this line means the plugin never loaded.
//
// P1.2 scope: this module owns the GameThreadDispatcher lifecycle
// (start/stop). All future UObject / editor API access routes through that one
// dispatcher (packages/bridge/AGENTS.md §Transport). HTTP listener (P1.3),
// instance lock, and tool dispatch land in later phases.
//
// Startup/shutdown are idempotent: ShutdownModule guards against
// double-teardown and repeated module reloads (Live Coding / hot reload) never
// crash.
#include "Modules/ModuleManager.h"

#include "UnrealOpenMcpLog.h"
#include "Bridge/UnrealOpenMcpBridgeSession.h"
#include "Dispatch/UnrealOpenMcpGameThreadDispatcher.h"

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

		// Construct the single game-thread dispatcher before any code path
		// that needs it. The HTTP server (P1.3) and every tool handler route
		// UObject/editor access through Dispatcher->EnqueueAsync — there is no
		// second dispatch path. StartupModule runs on the game thread, so this
		// construction is safe.
		Dispatcher = MakeUnique<FUnrealOpenMcpGameThreadDispatcher>();

		// Manual smoke (P1.2 verification plan: "log a dispatched lambda from
		// Editor module boot"). Runs inline because we are on the game thread;
		// the assertion inside proves the dispatch path works end-to-end
		// before P1.3 hangs /ping off it. Fire-and-forget — the result is
		// observed only via the Output Log line it emits.
		Dispatcher->Enqueue([]()
		{
			UE_LOG(
				LogUnrealOpenMcp,
				Log,
				TEXT("[Unreal Open MCP] game-thread dispatcher ready (dispatch smoke: body ran %s)"),
				IsInGameThread() ? TEXT("on the game thread") : TEXT("OFF the game thread")
			);
		});
	}

	virtual void ShutdownModule() override
	{
		// Idempotent: module shutdown may run after a reload cycle or during
		// editor teardown. Keep this path side-effect-free.
		UE_LOG(LogUnrealOpenMcp, Log, TEXT("[Unreal Open MCP] plugin shutting down"));

		// Tear down the dispatcher before the module goes away. After this,
		// any racing EnqueueAsync resolves immediately with
		// DispatcherShutdown instead of burning its timeout; Enqueue is a
		// silent no-op. The unique ptr then drops the instance. Safe to call
		// on the game thread during ShutdownModule.
		if (Dispatcher.IsValid())
		{
			Dispatcher->Shutdown();
			Dispatcher.Reset();
		}
	}

	/**
	 * Access the process-wide game-thread dispatcher. Returns null before
	 * StartupModule / after ShutdownModule. P1.3's HTTP server grabs this to
	 * marshal every tool body onto the game thread.
	 */
	FUnrealOpenMcpGameThreadDispatcher* GetDispatcher() const { return Dispatcher.Get(); }

private:
	// Owned. Constructed in StartupModule, torn down in ShutdownModule. The
	// dispatcher itself lives in the Runtime module (packaging-safe); only its
	// lifecycle is owned here, preserving the Editor→Runtime boundary.
	TUniquePtr<FUnrealOpenMcpGameThreadDispatcher> Dispatcher;
};

IMPLEMENT_MODULE(FUnrealOpenMcpEditorModule, UnrealOpenMcpEditor)
