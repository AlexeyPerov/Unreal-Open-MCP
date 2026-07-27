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
//
// P3.5 added a PRIVATE dependency on UnrealOpenMcpVerify — the gate flow
// (GatePolicy.Execute → VerifyGateAdapter → FVerifyRunner) consults verify's
// runner + CheckpointFingerprint / VerifyResult / IssueKey types. The
// dependency direction is one-way: bridge → verify, never the reverse (per
// packages/verify/AGENTS.md). PRIVATE because the bridge's public headers do
// not surface verify types — only the gate implementation files include
// verify headers.
//
// P4.1 added PRIVATE deps on AssetRegistry + AssetTools — the asset read
// family (asset_find / asset_get_data) queries the AssetRegistry directly
// (IAssetRegistry::GetAssets + FARFilter + FAssetData) and uses
// UEditorAssetLibrary (AssetTools module) for the get-data path-or-name
// probe (DoesAssetExist / FindAssetData).
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
			// P3.5 — gate policy. The bridge wraps every mutating dispatch in
			// FUnrealOpenMcpGatePolicy::Execute, which consults
			// FUnrealOpenMcpVerifyGateAdapter → FVerifyRunner for the
			// checkpoint / validate / delta cycle. The verify module owns the
			// rule registry (broken_soft_references / missing_blueprint_parent
			// / compile_errors in P3.2–P3.4) and the CheckpointFingerprint /
			// VerifyResult / IssueKey types. PRIVATE: verify types do not
			// surface in any bridge public header.
			"UnrealOpenMcpVerify",
			// P4.1 — asset read family (asset_find / asset_get_data). The
			// AssetRegistry module owns IAssetRegistry + FARFilter + FAssetData
			// (the query surface asset_find reads against); AssetTools owns
			// IAssetTools / FAssetToolsModule (the import task surface).
			// Both are first-party editor modules.
			"AssetRegistry",
			"AssetTools",
			// EditorScriptingUtilities owns UEditorAssetLibrary
			// (DoesAssetExist / LoadAsset / DuplicateAsset / RenameAsset /
			// MakeDirectory / FindAssetData / SaveLoadedAsset), which the asset
			// and material families use heavily. It was previously misattributed
			// to AssetTools in a comment and never declared, so
			// #include "EditorAssetLibrary.h" could not resolve. It ships as an
			// engine PLUGIN, so it is also listed in UnrealOpenMCP.uplugin's
			// "Plugins" array.
			"EditorScriptingUtilities",
			// P4.3 — material tools. MaterialEditor owns UMaterialEditingLibrary
			// (the editor-only scalar/vector/texture parameter read/write +
			// UpdateMaterialInstance surface); UMaterialInstanceConstantFactoryNew
			// lives in UnrealEd (already a dep) and the UMaterial* /
			// UMaterialInstanceConstant / UTexture classes live in Engine
			// (already a dep). First-party editor module.
			"MaterialEditor",
			// P5.5 — screenshot family (screenshot_viewport / _game_view /
			// _camera / _isolated). FImageUtils::PNGCompressImageArray drives
			// the opaque-PNG encode of the captured FColor buffer (ImageWrapper
			// module); FViewport::ReadPixels + render-target read-back for the
			// SceneCapture2D path pull types from RHI / RenderCore. First-party
			// engine modules; editor-only (the module is Type: Editor).
			"ImageWrapper",
			"RenderCore",
			"RHI",
			// P6.1 — Blueprint family (blueprint_create / blueprint_get). The
			// Kismet module owns FKismetEditorUtilities (CreateBlueprint /
			// CanCreateBlueprintOfClass) + FKismetNameValidator (name
			// well-formedness). UBlueprint / UBlueprintGeneratedClass / the SCS
			// node surface live in Engine (already a dep); the EdGraph + K2
			// event node types live in Engine too. AssetRegistry (already a
			// dep) owns FAssetRegistryModule::AssetCreated. Editor-only.
			"Kismet",
			});

		// P7.3 — source_compile prefers Live Coding to patch a running editor
		// when interactive (the only way to apply C++ changes without relinking
		// the locked module DLL). The LiveCoding module is Windows-only
		// (Engine/Source/Developer/Windows/LiveCoding); gate the dependency +
		// the WITH_UNREAL_MCP_LIVE_CODING define so non-Windows builds (and the
		// UBT fallback path) compile cleanly without it. When the guard is 0,
		// the source_compile handler unconditionally takes the UBT path.
		if (Target.Platform == UnrealBuildTool.UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
			PublicDefinitions.Add("WITH_UNREAL_MCP_LIVE_CODING=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_UNREAL_MCP_LIVE_CODING=0");
		}

		// The per-session bearer token must come from the OS cryptographic RNG
		// (FUnrealOpenMcpBridgeAuthToken::Generate). On Windows that is
		// BCryptGenRandom, which needs bcrypt.lib. Mac/Linux read /dev/urandom
		// and need no extra library.
		if (Target.Platform == UnrealBuildTool.UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("bcrypt.lib");
		}

		// P3.5 scope: gate policy wired at the dispatch boundary. P4.1 adds
		// AssetRegistry + AssetTools for the asset read family; P4.3 adds
		// MaterialEditor for the material tools. P4.4 (asset_import) needs no
		// new module — IAssetTools::ImportAssetTasks + UAssetImportTask come
		// from AssetTools + UnrealEd (already deps), and FGCObjectScopeGuard
		// from CoreUObject. P7.3 adds the LiveCoding module on Windows only
		// (source_compile's interactive fast path — non-Windows hosts fall back
		// to UBT, and the WITH_UNREAL_MCP_LIVE_CODING guard compiles the LC
		// branch out cleanly). The dependency surface stays minimal so the
		// Editor/Runtime boundary guard (P1.8) stays green and later phases
		// add deps as they add features.
	}
}
