// Automation-spec-only module; no startup/shutdown logic needed. Each spec
// .cpp under this folder is guarded by WITH_DEV_AUTOMATION_TESTS and
// self-registers via IMPLEMENT_SIMPLE_AUTOMATION_TEST / BEGIN_DEFINE_SPEC.
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, UnrealOpenMcpVerifyTests)
