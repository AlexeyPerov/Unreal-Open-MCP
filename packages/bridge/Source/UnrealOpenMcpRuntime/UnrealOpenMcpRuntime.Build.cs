// UnrealOpenMcpRuntime — runtime-safe bridge infrastructure.
//
// Per docs/architecture.md (Editor / Runtime boundary): Runtime may NEVER
// reference Editor code; Editor may reference Runtime. This module is the
// packaging home for types that may ship to packaged builds when explicitly
// opted in later. It currently carries:
//   - The shared log category (UnrealOpenMcpLog).
//   - The game-thread dispatcher (P1.2) — packaging-safe, no editor hooks.
//   - The self-contained SHA-256 + instance-port resolver (P1.4) — pure math
//     + path handling, no editor APIs. The instance-lock file writer lives in
//     the Editor module because it touches editor lifecycle (heartbeat
//     ticker), but the formula + lock path derivation stay here so a future
//     packaged commandlet can derive its port without editor code.

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
