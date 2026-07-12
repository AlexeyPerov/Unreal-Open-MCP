// Runtime module for the Unreal Open MCP plugin.
//
// Type `Runtime`: loads in the editor, PIE, Standalone and packaged builds.
// It is the packaging home for runtime-safe types (log category today; shared
// infra in later phases). In P1.1 it carries no own bootstrap — the Editor
// module owns bridge lifecycle. The Editor/Runtime boundary invariant
// (docs/architecture.md) is one-directional: Editor may reference Runtime;
// Runtime must never reference Editor.
#include "Modules/ModuleManager.h"

class FUnrealOpenMcpRuntimeModule : public IModuleInterface
{
};

IMPLEMENT_MODULE(FUnrealOpenMcpRuntimeModule, UnrealOpenMcpRuntime)
