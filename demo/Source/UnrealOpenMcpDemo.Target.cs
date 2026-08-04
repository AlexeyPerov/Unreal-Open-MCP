// Copyright (c) 2026 Alexey Perov. Licensed under the MIT License.
// See the LICENSE file in the repository root for more information.

using UnrealBuildTool;
using System.Collections.Generic;

public class UnrealOpenMcpDemoTarget : TargetRules
{
	public UnrealOpenMcpDemoTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		ExtraModuleNames.Add("UnrealOpenMcpDemo");
	}
}
