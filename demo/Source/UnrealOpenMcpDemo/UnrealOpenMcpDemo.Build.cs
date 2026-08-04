// Copyright (c) 2026 Alexey Perov. Licensed under the MIT License.
// See the LICENSE file in the repository root for more information.
//
// Primary game module for the Unreal Open MCP demo project.
//
// Minimal C++ runtime module (Type Runtime) so the project builds as code and
// can host the bridge plugin. It carries no game logic of its own — it exists
// so E2E smokes and validation have a stable, first-party project root that
// compiles under UE 5.6+ (developed against 5.8).

using UnrealBuildTool;

public class UnrealOpenMcpDemo : ModuleRules
{
	public UnrealOpenMcpDemo(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			// The .uproject's AdditionalDependencies already lists Engine, but
			// the module rules are the authoritative build-graph input, so we
			// declare it here too — a runtime game module with no Engine dep
			// would not link.
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
		});
	}
}
