// UnrealOpenMcpVerifyTests — Automation-spec-only module for verify.
//
// Per packages/verify/AGENTS.md §Verification: the narrowest Automation spec
// lives here. The module is Type: Editor (Automation specs run in the editor
// test runner), compiles WITH_DEV_AUTOMATION_TESTS only via the spec guards in
// each .cpp, and carries no startup/shutdown logic.
//
// Depends PUBLICLY on UnrealOpenMcpVerify so the contract headers under
// Public/Core/ and Public/Fixes/ resolve across the .dll boundary.
using System.IO;
using UnrealBuildTool;

public class UnrealOpenMcpVerifyTests : ModuleRules
{
	public UnrealOpenMcpVerifyTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			// Verify must be PUBLIC: specs include the contract headers under
			// Public/Core/ and Public/Fixes/ across the .dll boundary.
			"UnrealOpenMcpVerify",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			// P3.2: the BrokenSoftReferences spec loads synthetic packages via
			// LoadPackage and walks them via FSoftObjectPath / Asset Registry, so
			// mirror the verify module's private deps. Specs are editor-only.
			"Engine",
			"AssetRegistry",
		});

		// Reach the verify module's PRIVATE headers (the runner implementation
		// under Private/Core/VerifyRunner.cpp) so Automation specs can drive
		// its internals directly. The contract types are PUBLIC, so they are
		// already on the include path via the PublicDependency above.
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "UnrealOpenMcpVerify", "Private"));
	}
}
