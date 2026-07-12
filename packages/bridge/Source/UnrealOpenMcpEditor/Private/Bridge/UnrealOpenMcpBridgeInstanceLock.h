// Instance lock + heartbeat file for the Unreal Open MCP bridge.
//
// Ports Unity Open MCP's BridgeInstanceLock
// (packages/bridge/Editor/Bridge/BridgeInstanceLock.cs) to Unreal C++.
// Fidelity: copy — the file shape and lifecycle MUST stay parity-compatible
// with the TS reader (mcp-server/src/instance-discovery.ts) so the MCP server
// can discover the right port per project without an HTTP round-trip and
// without sharing config with the bridge.
//
// Each running bridge instance owns a lock file at:
//   ~/.unreal-open-mcp/instances/<sha256(projectPath)>.json
// The file doubles as the heartbeat: it carries the current editor state and
// is rewritten by Acquire / UpdateState. The MCP server reads it read-only.
//
// Lifecycle (mirrors Unity):
//   - Acquire(projectPath, port)  — called once when the listener starts.
//     Sweeps stale locks (PID no longer alive) across ALL instances, then
//     writes this instance's lock.
//   - UpdateState(state, ...)     — rewrite with fresh editor state. No-op
//     until Acquire has run. (A future heartbeat ticker will call this on a
//     0.5s cadence — P5.7.)
//   - Release()                   — delete the lock on graceful shutdown.
//     Best-effort: a crashed editor leaves a stale lock the next Acquire
//     cleans up.
//
// Why an instance type (not a static singleton like Unity): Unreal's module
// reload model and testability both favor an owned instance. The Editor module
// owns one, passes it by reference where needed, and reconstructs it on hot
// reload. On-disk continuity (PID + projectHash) is what makes stale recovery
// work across reloads, not in-process identity.
//
// authToken: deferred to P5.6 per the P1.4 plan. The field is OMITTED from the
// JSON (not written as null/empty) — absence is pinned in the spec. When P5.6
// lands, it will be added between port and projectPath (Unity's order) and the
// TS reader already tolerates its absence.
//
// Threading: Acquire/UpdateState/Release may be called from the game thread or
// the listener worker. File I/O is atomic via rename; concurrent UpdateState
// calls are last-writer-wins and the heartbeat is the only steady-state writer.
// The .tmp file path is per-PID to avoid collisions.
#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"

/**
 * Instance lock + heartbeat file.
 *
 * One instance per bridge (owned by FUnrealOpenMcpEditorModule). All file I/O
 * is best-effort: a failed write is logged and never tears down the editor.
 */
class UNREALOPENMCPEDITOR_API FUnrealOpenMcpBridgeInstanceLock
{
public:
	FUnrealOpenMcpBridgeInstanceLock() = default;
	~FUnrealOpenMcpBridgeInstanceLock();

	// State values written into the lock. Mirror the TS-side InstanceState
	// type in instance-discovery.ts so both sides agree on the vocabulary.
	static constexpr const TCHAR* StateIdle() { return TEXT("idle"); }
	static constexpr const TCHAR* StateCompiling() { return TEXT("compiling"); }
	static constexpr const TCHAR* StateReloading() { return TEXT("reloading"); }
	static constexpr const TCHAR* StateEnteringPlaymode() { return TEXT("entering_playmode"); }
	static constexpr const TCHAR* StatePlaying() { return TEXT("playing"); }
	static constexpr const TCHAR* StateExitingPlaymode() { return TEXT("exiting_playmode"); }

	/** True between a successful Acquire and Release. */
	bool IsAcquired() const { return bAcquired; }

	/** The project path the lock was acquired for (empty when not acquired). */
	const FString& GetCurrentProjectPath() const { return AcquiredProjectPath; }

	/** The port written into the lock (0 when not acquired). */
	int32 GetCurrentPort() const { return AcquiredPort; }

	/**
	 * Write the initial lock and sweep stale locks. Safe to call on the
	 * listener worker thread — no Unreal APIs are touched.
	 *
	 * @param ProjectPath    absolute project dir; surfaces in the lock JSON.
	 * @param Port           port the HTTP listener is bound to.
	 * @param BridgeVersion  bridge/plugin version string (from BridgeSession).
	 * @param UnrealVersion  engine version string.
	 * @param InstancesDirOverride  test-only sandbox for the instances dir.
	 */
	void Acquire(
		const FString& ProjectPath,
		int32 Port,
		const FString& BridgeVersion,
		const FString& UnrealVersion,
		const FString& InstancesDirOverride = FString());

	/**
	 * Rewrite the lock with fresh editor state. No-op until Acquire has run.
	 * Best-effort: a write failure is logged and swallowed.
	 */
	void UpdateState(const FString& State, bool bIsPlaying, bool bIsCompiling);

	/**
	 * Delete the lock on graceful shutdown. Idempotent. Best-effort.
	 */
	void Release();

	/**
	 * Snapshot of the current lock file content as a JSON string. Used by a
	 * future /instance HTTP endpoint so the MCP server can verify the live
	 * bridge against the on-disk lock. Returns empty when no lock is held or
	 * the file can't be read.
	 */
	FString ReadCurrentJson() const;

	/**
	 * Lightweight read-only view of the diagnostic fields. Pure (no file I/O,
	 * no Unreal APIs) so it is unit-testable. Valid=false on malformed input.
	 */
	struct FLockSnapshot
	{
		bool bValid = false;
		int32 Pid = 0;
		int32 Port = 0;
		FString State;
		FString UpdatedAt;
		FString HeartbeatAt;
	};
	static FLockSnapshot TryParseSnapshot(const FString& Json);

private:
	/** Build the lock JSON string for the current state. */
	FString BuildJson(const FString& State, bool bIsPlaying, bool bIsCompiling, const FDateTime& Now) const;

	/** Write the JSON to the lock path atomically (.tmp + rename). */
	void WriteLock(const FString& State, bool bIsPlaying, bool bIsCompiling, const FDateTime& Now);

	/** Ensure the instances dir exists. */
	void EnsureInstancesDir() const;

	/** Scan the instances dir and delete locks whose PID is no longer alive. */
	void SweepStaleLocks() const;

	/** Absolute lock path for the acquired project. Empty when not acquired. */
	FString GetAcquiredLockPath() const;

	/** True when a process with the given PID is still running. */
	static bool IsPidAlive(uint32 Pid);

	/** Hand-rolled JSON field extractors (no JSON dependency in the bridge). */
	static int32 ExtractInt(const FString& Json, const FString& Key);
	static FString ExtractString(const FString& Json, const FString& Key);

	/** True between a successful Acquire and Release. */
	bool bAcquired = false;
	FString AcquiredProjectPath;
	FString AcquiredProjectHash;
	int32 AcquiredPort = 0;
	uint32 Pid = 0;
	FDateTime StartedAt;
	FString BridgeVersionForLock;
	FString UnrealVersionForLock;
	FString InstancesDirOverrideForLock;
};
