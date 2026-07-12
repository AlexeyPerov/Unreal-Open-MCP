// Editor module for the Unreal Open MCP plugin.
//
// Adapts Unity Open MCP's bridge lifecycle ([InitializeOnLoad] static ctor +
// EditorApplication.quitting) to Unreal's IModuleInterface: StartupModule owns
// boot + diagnostics, ShutdownModule owns teardown. The startup log line is
// the bridge's diagnostic proof of life — when triaging a missing /ping, the
// absence of this line means the plugin never loaded.
//
// Lifecycle ownership:
//   - GameThreadDispatcher (P1.2) — single marshaling path for UObject/editor
//     access. Owned here; passed by reference into the HTTP server.
//   - BridgeHttpServer (P1.3) — loopback HTTP listener with GET /ping. Owned
//     here; started with a resolved port, stopped before the dispatcher so the
//     server's pending /ping dispatch fails fast with DispatcherShutdown
//     instead of a hang.
//
// Startup/shutdown are idempotent: ShutdownModule guards against
// double-teardown and repeated module reloads (Live Coding / hot reload) never
// crash.
#include "Modules/ModuleManager.h"

#include "UnrealOpenMcpLog.h"
#include "Bridge/UnrealOpenMcpBridgeHttpServer.h"
#include "Bridge/UnrealOpenMcpBridgeSession.h"
#include "Dispatch/UnrealOpenMcpGameThreadDispatcher.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

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

		// Start the loopback HTTP server. The dispatcher is passed by reference
		// so the server's /ping handler can marshal its small body onto the
		// game thread (packages/bridge/AGENTS.md §Transport — never call editor
		// APIs from the listener worker thread). ResolvePort honors the
		// UNREAL_OPEN_MCP_BRIDGE_PORT env override; the deterministic hash
		// resolver lands in P1.4.
		HttpServer = MakeUnique<FUnrealOpenMcpBridgeHttpServer>(*Dispatcher);
		const uint16 Port = FUnrealOpenMcpBridgeHttpServer::ResolvePort();
		const FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		if (!HttpServer->Start(Port, ProjectPath))
		{
			// A failed bind is logged but NOT fatal — the editor still runs,
			// and the operator can free the port and reload the plugin. The
			// dispatcher stays up so a later Start (hot reload after freeing
			// the port) works without reconstructing it.
			UE_LOG(
				LogUnrealOpenMcp,
				Error,
				TEXT("[Unreal Open MCP] bridge HTTP server failed to start: %s"),
				*HttpServer->GetLastStartError());
		}
	}

	virtual void ShutdownModule() override
	{
		// Idempotent: module shutdown may run after a reload cycle or during
		// editor teardown. Keep this path side-effect-free.
		UE_LOG(LogUnrealOpenMcp, Log, TEXT("[Unreal Open MCP] plugin shutting down"));

		// Tear down the HTTP server BEFORE the dispatcher: in-flight /ping
		// dispatches must resolve with DispatcherShutdown (fail-fast) rather
		// than hanging on a dispatcher that no longer exists. The HTTP server
		// joins its listener thread inside Stop().
		if (HttpServer.IsValid())
		{
			HttpServer->Stop();
			HttpServer.Reset();
		}

		// Tear down the dispatcher. After this, any racing EnqueueAsync
		// resolves immediately with DispatcherShutdown instead of burning its
		// timeout; Enqueue is a silent no-op. The unique ptr then drops the
		// instance. Safe to call on the game thread during ShutdownModule.
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

	/**
	 * Access the bridge HTTP server. Returns null before StartupModule / after
	 * ShutdownModule. Exposed for future editor UI (status panel) and tests.
	 */
	FUnrealOpenMcpBridgeHttpServer* GetHttpServer() const { return HttpServer.Get(); }

private:
	// Owned. Constructed in StartupModule, torn down in ShutdownModule. The
	// dispatcher itself lives in the Runtime module (packaging-safe); only its
	// lifecycle is owned here, preserving the Editor→Runtime boundary.
	TUniquePtr<FUnrealOpenMcpGameThreadDispatcher> Dispatcher;

	// Owned. Constructed after the dispatcher in StartupModule, torn down
	// before it in ShutdownModule. The HTTP server holds a reference to the
	// dispatcher, so the destruction order below (HttpServer first, Dispatcher
	// second) is load-bearing.
	TUniquePtr<FUnrealOpenMcpBridgeHttpServer> HttpServer;
};

IMPLEMENT_MODULE(FUnrealOpenMcpEditorModule, UnrealOpenMcpEditor)
