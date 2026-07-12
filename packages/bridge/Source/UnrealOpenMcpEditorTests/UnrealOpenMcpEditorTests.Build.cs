// UnrealOpenMcpEditorTests — Automation-spec-only module for the bridge.
//
// Per packages/bridge/AGENTS.md §Verification: the narrowest Automation spec
// lives here. The module is Type: Editor (Automation specs run in the editor
// test runner), compiles WITH_DEV_AUTOMATION_TESTS only via the spec guards in
// each .cpp, and carries no startup/shutdown logic.
//
// Depends PUBLICLY on UnrealOpenMcpRuntime so the dispatcher headers
// (Public/Dispatch/UnrealOpenMcpGameThreadDispatcher.h) resolve. Editor module
// is a Private dependency because the specs do not yet drive Editor-only
// internals (P1.2 covers the dispatcher contract, which lives in Runtime); it
// is wired now so later phases' specs reach Editor headers without a churn
// round.
using System.IO;
using UnrealBuildTool;

public class UnrealOpenMcpEditorTests : ModuleRules
{
	public UnrealOpenMcpEditorTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			// Runtime must be PUBLIC: specs include
			// Runtime's public dispatcher header across the .dll boundary, and
			// the Editor→Runtime invariant is one-directional.
			"UnrealOpenMcpRuntime",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			// Editor module: reach its private headers (e.g. BridgeSession.h
			// under Private/Bridge/) for future specs.
			"UnrealOpenMcpEditor",
			// P1.3 — the ping spec drives the loopback HTTP listener (FTcpSocketBuilder,
			// raw socket Recv) and reads the bound port back from FSocket.
			"Sockets",
			"Networking",
		});

		// Reach the editor module's PRIVATE headers so Automation specs can
		// drive its internals directly. The runtime path is PUBLIC, so it is
		// already on the include path via the PublicDependency above.
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "UnrealOpenMcpEditor", "Private"));
	}
}
