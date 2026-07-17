// Verify module for Unreal Open MCP.
//
// Type `Editor`: editor-only health checks. Carries the verify rule and fix
// contracts that the bridge gate flow (checkpoint → mutate → validate →
// delta) dispatches into.
//
// P3.1 shipped pure contract types + the runner shell. P3.2 wired the first
// concrete rule family (broken_soft_references) into the runner's
// RegisterDefaults. StartupModule keeps the default rule/fix registrations
// idempotent (the bridge hard-depends on verify in P3.5; an editor reload
// must not double-register).
//
// Per packages/verify/AGENTS.md: verify never depends on the bridge. The
// dependency direction is bridge → verify, never the reverse.
#include "Modules/ModuleManager.h"

#include "Core/VerifyRunner.h"
#include "Fixes/FixProviderRegistry.h"

class FUnrealOpenMcpVerifyModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Idempotent: RegisterDefaults short-circuits when the registry is
		// already populated, so a hot reload never double-registers. The bridge
		// also calls EnsureDefaultsRegistered() from P3.5 onward before its
		// first gate run, but starting it here keeps a standalone editor (no
		// bridge) consistent with the bridge-driven path.
		FVerifyRunner::EnsureDefaultsRegistered();
		FFixProviderRegistry::EnsureDefaultsRegistered();
	}
};

IMPLEMENT_MODULE(FUnrealOpenMcpVerifyModule, UnrealOpenMcpVerify)
