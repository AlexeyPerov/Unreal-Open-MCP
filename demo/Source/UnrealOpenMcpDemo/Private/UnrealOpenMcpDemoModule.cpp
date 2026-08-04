// Copyright (c) 2026 Alexey Perov. Licensed under the MIT License.
// See the LICENSE file in the repository root for more information.
//
// Primary game module implementation for the Unreal Open MCP demo project.
//
// Empty by design — the module exists so the project is a code project (not
// Blueprint-only) and can host the bridge plugin under Plugins/. Game logic
// is out of scope; validation scenarios drive the editor through MCP tools.

#include "Modules/ModuleManager.h"

class FUnrealOpenMcpDemoModule : public IModuleInterface
{
};

IMPLEMENT_MODULE(FUnrealOpenMcpDemoModule, UnrealOpenMcpDemo)
