// UnrealOpenMcpEditor — editor-only bridge lifecycle owner.
//
// Per packages/bridge/AGENTS.md and docs/architecture.md (Editor / Runtime
// boundary): this module depends on Runtime PUBLICLY so its public headers
// (UnrealOpenMcpLog.h, Dispatch/UnrealOpenMcpGameThreadDispatcher.h,
// Bridge/UnrealOpenMcpInstancePortResolver.h, Crypto/UnrealOpenMcpSha256.h)
// are visible across the .dll boundary. In P1.1 the editor module owned
// plugin boot logging only; P1.3 adds the loopback HTTP server + /ping health
// surface; P1.4 adds the instance lock + wires the deterministic port
// resolver into the HTTP server start; tool dispatch lands in P2.1.
using UnrealBuildTool;

public class UnrealOpenMcpEditor : ModuleRules
{
	public UnrealOpenMcpEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			// Runtime must be a PUBLIC dependency: this module references
			// Runtime's public headers (UnrealOpenMcpLog.h,
			// Dispatch/UnrealOpenMcpGameThreadDispatcher.h) and the boundary
			// invariant is Editor→Runtime, never the reverse.
			"UnrealOpenMcpRuntime",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			// Projects: IPluginManager for plugin descriptor/paths (future use).
			"Projects",
			// P1.3 — loopback HTTP bridge. Sockets + Networking expose the raw
			// socket + IPv4 endpoint + FTcpSocketBuilder APIs the own listener
			// needs; we deliberately do NOT use FHttpServerModule (binds all
			// interfaces).
			"Sockets",
			"Networking",
		});

		// P1.3 scope: HTTP server + /ping only. No Slate UI, no gate wiring.
		// Keep the dependency surface minimal so the Editor/Runtime boundary
		// guard (P1.8) stays green and later phases add deps as they add
		// features.
	}
}
