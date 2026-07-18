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
//
// P3.2 added private deps on Engine + AssetRegistry for the
// broken_soft_references rule scanner. The dependency invariant (verify never
// depends on an UnrealOpenMcp* module) is preserved.
//
// P3.4 added a Windows-only LiveCoding dep for the compile_errors rule's
// production status provider (reads ILiveCodingModule::IsEnabledForSession /
// HasStarted / IsCompiling — never Compile()). The LiveCoding module is
// Windows-only in stock UE (Engine/Source/Developer/Windows/LiveCoding), so
// the dep is gated to Win64 and the provider's #include is guarded by
// PLATFORM_WINDOWS. On other platforms the provider returns Clean (no
// hot-reload failure state to observe); the rule's behavior contract is still
// fully pinned by the Automation spec via the injected fake provider.
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
			// P3.2: the broken_soft_references rule loads packages and walks
			// UObjects (LoadPackage, GetObjectsWithOuter, FSoftObjectPath) and
			// looks up package existence via the Asset Registry. Engine and
			// AssetRegistry are first-party editor modules; verify still does
			// NOT depend on any UnrealOpenMcp* module (the bridge depends on
			// verify, never the reverse).
			"Engine",
			"AssetRegistry",
		});

		// P3.4: Windows-only LiveCoding dependency. The provider consults
		// ILiveCodingModule for read-only status (IsEnabledForSession /
		// HasStarted / IsCompiling); it NEVER calls Compile() — that is the
		// side effect the rule bans. On non-Windows platforms the provider's
		// #if PLATFORM_WINDOWS branch is excluded and it returns Clean.
		if (Target.Platform == UnrealBuildTool.UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}

		// Deliberately no UnrealOpenMcpEditor / UnrealOpenMcpRuntime dependency.
		// The verify contracts are standalone C++ types; the bridge will take a
		// dependency on this module in P3.5 when the gate flow is wired.
	}
}
