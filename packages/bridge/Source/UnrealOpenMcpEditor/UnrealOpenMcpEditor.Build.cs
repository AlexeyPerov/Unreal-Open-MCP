// UnrealOpenMcpEditor — editor-only bridge lifecycle owner.
//
// Per packages/bridge/AGENTS.md and docs/architecture.md (Editor / Runtime
// boundary): this module depends on Runtime PUBLICLY so its public headers
// (UnrealOpenMcpLog.h, Dispatch/UnrealOpenMcpGameThreadDispatcher.h,
// Bridge/UnrealOpenMcpInstancePortResolver.h, Crypto/UnrealOpenMcpSha256.h)
// are visible across the .dll boundary. P1.1 added plugin boot logging; P1.3
// added the loopback HTTP server + /ping health surface; P1.4 added the
// instance lock + wired the deterministic port resolver into the HTTP server
// start; P2.1 added POST /tools/{name} dispatch (tool registry, fair request
// queue, canonical {ok,result,error} envelope, echo stub tool); P2.2 added the
// first real typed tool family (actor tools — read-only find), which pulls in
// UnrealEd (GEditor/GetEditorWorldContext) and Json (FJsonObject arg parsing
// inside the tool handlers); P2.4 added actor_modify + object_modify, which
// reflect FProperty writes via FJsonObjectConverter (JsonUtilities module).
using UnrealBuildTool;

public class UnrealOpenMcpEditor : ModuleRules
{
	public UnrealOpenMcpEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			// Runtime must be a PUBLIC dependency: this module references
			// Runtime's public headers (UnrealOpenMcpLog.h,
			// Dispatch/UnrealOpenMcpGameThreadDispatcher.h) and the boundary
			// invariant is Editor→Runtime, never the reverse.
			"UnrealOpenMcpRuntime",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			// Projects: IPluginManager for plugin descriptor/paths (future use).
			"Projects",
			// P1.3 — loopback HTTP bridge. Sockets + Networking expose the raw
			// socket + IPv4 endpoint + FTcpSocketBuilder APIs the own listener
			// needs; we deliberately do NOT use FHttpServerModule (binds all
			// interfaces).
			"Sockets",
			"Networking",
			// P2.2 — actor tools resolve the editor world via GEditor
			// (GEditor->GetEditorWorldContext().World()). Editor.h lives in the
			// UnrealEd module. This is an editor-only module (Type: Editor in
			// the .uplugin), so the dependency does not run afoul of the
			// Editor/Runtime boundary guard (P1.8 scans Runtime only).
			"UnrealEd",
			// P2.2 — typed tool handlers parse the raw POST body into an
			// FJsonObject (TJsonReader + FJsonSerializer) and emit pre-serialized
			// JSON results. The Json module supplies FJsonObject + the reader/
			// writer.
			"Json",
			// P2.4 — actor_modify / object_modify reflect FProperty writes via
			// FJsonObjectConverter::JsonValueToUProperty (bool/int/float/string/
			// vector/rotator/color/enum-by-name). The JsonUtilities module owns
			// FJsonObjectConverter; the Json module alone does not expose it.
			"JsonUtilities",
		});

		// P2.4 scope: HTTP server + /ping + tool dispatch + actor find/create/
		// modify + object_modify. No Slate UI, no gate wiring. Keep the
		// dependency surface minimal so the Editor/Runtime boundary guard
		// (P1.8) stays green and later phases add deps as they add features.
	}
}
