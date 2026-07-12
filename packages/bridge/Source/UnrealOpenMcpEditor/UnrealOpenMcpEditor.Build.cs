// UnrealOpenMcpEditor — editor-only bridge lifecycle owner.
//
// Per packages/bridge/AGENTS.md and docs/architecture.md (Editor / Runtime
// boundary): this module depends on Runtime PUBLICLY so its public headers
// (UnrealOpenMcpLog.h) are visible across the .dll boundary. In P1.1 the
// editor module owns plugin boot logging only; HTTP server, dispatcher, and
// tool dispatch land in later phases.
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
			// Runtime's public headers (UnrealOpenMcpLog.h) and the boundary
			// invariant is Editor→Runtime, never the reverse.
			"UnrealOpenMcpRuntime",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			// Projects: IPluginManager for plugin descriptor/paths (future use).
			"Projects",
		});

		// P1.1 scope: no HTTP server, no Slate UI, no gate wiring. Keep the
		// dependency surface minimal so the Editor/Runtime boundary guard
		// (P1.8) starts green and later phases add deps as they add features.
	}
}
