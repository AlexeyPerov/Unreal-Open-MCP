// UnrealOpenMcpRuntime — runtime-safe bridge infrastructure.
//
// Per docs/architecture.md (Editor / Runtime boundary): Runtime may NEVER
// reference Editor code; Editor may reference Runtime. This module exists from
// day one (P1.1) as the packaging home for types that may ship to packaged
// builds when explicitly opted in later. In P1.1 it carries only the log
// category and module stub — no engine-agnostic subsystems yet.

using UnrealBuildTool;

public class UnrealOpenMcpRuntime : ModuleRules
{
	public UnrealOpenMcpRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		// Keep this module free of UnrealEd / editor-only dependencies. The
		// P1.8 Editor/Runtime boundary CI guard asserts that invariant; do not
		// add editor modules here without narrowing the architecture decision.
	}
}
