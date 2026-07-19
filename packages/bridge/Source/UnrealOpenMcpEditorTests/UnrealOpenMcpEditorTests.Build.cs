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
			// under Private/Bridge/, Tools/UnrealOpenMcpActorTools.h under
			// Private/Tools/, Gate/UnrealOpenMcpGatePolicy.h under
			// Private/Gate/) for specs.
			"UnrealOpenMcpEditor",
			// P1.3 — the ping spec drives the loopback HTTP listener (FTcpSocketBuilder,
			// raw socket Recv) and reads the bound port back from FSocket.
			"Sockets",
			"Networking",
			// P2.2 — the actor-find spec parses the tool's JSON output
			// (FJsonObject / JsonReader) to assert on the actors[] payload, and
			// drives the editor world (GEditor) to spawn test actors.
			"Json",
			"UnrealEd",
			// P3.5 — the gate-policy spec drives the verify rule registry to
			// install stub rules (emitting known issues so the gate delta has a
			// deterministic shape). The bridge hard-depends on verify; the
			// specs need the same module so they can reach FVerifyRunner /
			// IVerifyRule.
			"UnrealOpenMcpVerify",
			// P4.1 — the asset-find / asset-get-data specs query the live
			// AssetRegistry (IAssetRegistry + FARFilter + FAssetData) and use
			// UEditorAssetLibrary (AssetTools module) for the get-data probe,
			// mirroring the editor module's P4.1 private deps.
			"AssetRegistry",
			"AssetTools",
			// P4.3 — the material-tools spec creates a UMaterialInstanceConstant
			// from an engine parent material and reads/writes its parameters via
			// UMaterialEditingLibrary (MaterialEditor module), mirroring the
			// editor module's P4.3 private dep.
			"MaterialEditor",
		});

		// Reach the editor module's PRIVATE headers so Automation specs can
		// drive its internals directly. The runtime path is PUBLIC, so it is
		// already on the include path via the PublicDependency above.
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "UnrealOpenMcpEditor", "Private"));
	}
}
