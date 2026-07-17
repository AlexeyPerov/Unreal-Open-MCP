// UnrealOpenMcpVerify — scoped health-check module for Unreal Open MCP.
//
// Per packages/verify/AGENTS.md and docs/architecture.md (Editor / Runtime
// boundary): this module owns the verify rule and fix contracts that the
// bridge gate flow (checkpoint → mutate → validate → delta) dispatches into.
//
// The load-bearing invariant is one-directional: **the bridge depends on
// verify; verify never depends on the bridge.** This Build.cs therefore lists
// NO UnrealOpenMcp* module dependencies — verify must stay usable standalone
// (the MCP-side offline scanner can read its issue codes without a live
// editor, and the gate will soft/hard-depend on it from P3.5 onward).
//
// P3.1 scope: pure contract types only. No concrete rule scanners, no Asset
// Registry walks, no bridge gate wiring — those land in P3.2–P3.4 / P3.7.
using UnrealBuildTool;

public class UnrealOpenMcpVerify : ModuleRules
{
	public UnrealOpenMcpVerify(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
		});

		// Deliberately no UnrealOpenMcpEditor / UnrealOpenMcpRuntime dependency.
		// The verify contracts are standalone C++ types; the bridge will take a
		// dependency on this module in P3.5 when the gate flow is wired.
	}
}
