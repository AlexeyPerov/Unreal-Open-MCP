// Project settings reader for the bridge.
//
// Adapts Unity Open MCP's BridgeProjectSettings
// (packages/bridge/Editor/Bridge/BridgeProjectSettings.cs) — narrowed to the
// auth-relevant fields (authMode + bindAddress). Fidelity: adapt — the schema
// path is `.unreal-open-mcp` (not Unity's `.unity-open-mcp`) and the reader is
// an instance type (not Unity's static singleton) so tests can inject a temp
// project root.
//
// Settings file: <project-root>/.unreal-open-mcp/settings.json
// Schema v1 (only the fields the bridge reads today):
//   {
//     "authMode": "none" | "required",     // default "none"
//     "bindAddress": "127.0.0.1" | "0.0.0.0" // default "127.0.0.1"
//   }
//
// Load semantics (mirror Unity):
//   - Missing file → defaults (authMode "none", bindAddress loopback).
//   - Unparseable JSON → defaults + a warning log (never crash the bridge).
//   - Invalid authMode/bindAddress value → coerce to the safe default
//     (authMode "none", bindAddress loopback). Coercion happens here so the
//     HTTP server and the bind decision always see a canonical value.
//
// Atomic write: a setter writes to `<path>.tmp` then renames (same pattern as
// the instance lock) so a crash mid-write never leaves a half-written file.
//
// Thread-safety: Load is lazy + idempotent; getters read cached fields. The
// bridge reads settings once at StartupModule (single-threaded) and the setters
// are not wired to a live UI in this phase, so no concurrent-access guard is
// needed yet. If a future settings tab lands, add a FCriticalSection around the
// load/mutate path.
#pragma once

#include "CoreMinimal.h"

/**
 * Auth-relevant project settings for the bridge. One instance per bridge
 * (owned by FUnrealOpenMcpEditorModule). Defaults are applied when the file is
 * missing or holds invalid values.
 */
class UNREALOPENMCPEDITOR_API FUnrealOpenMcpBridgeProjectSettings
{
public:
	FUnrealOpenMcpBridgeProjectSettings() = default;

	/** Settings file name under <project-root>/.unreal-open-mcp/. */
	static constexpr const TCHAR* FileName() { return TEXT("settings.json"); }

	/**
	 * Canonical authMode: the file value when valid, else the default ("none").
	 * Loads lazily on first access.
	 */
	FString GetAuthMode();

	/**
	 * Canonical bindAddress: the file value when valid, else loopback. Loads
	 * lazily on first access.
	 */
	FString GetBindAddress();

	/**
	 * Resolve the absolute settings path for a project root:
	 *   <projectRoot>/.unreal-open-mcp/settings.json
	 */
	static FString ResolveSettingsPath(const FString& ProjectRoot);

	/**
	 * Bind this instance to a project root + (test-only) override. Called once
	 * at StartupModule. ProjectRoot empty → no file is read (defaults apply).
	 */
	void BindToProject(const FString& ProjectRoot);

private:
	/** Load from disk if not already loaded. Idempotent. */
	void EnsureLoaded();

	/** Parse the JSON body and canonicalize the two fields. */
	void ApplyJson(const FString& JsonBody);

	FString ProjectRootPath;
	bool bLoaded = false;
	FString AuthMode;
	FString BindAddress;
};
