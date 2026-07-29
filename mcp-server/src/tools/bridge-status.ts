import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P5.7 — operator / recovery-oriented bridge health snapshot. A thin wrapper
// over the existing instance-lock classifier (`classifyInstance` in
// instance-discovery.ts) plus a single /ping probe. It exists so an operator
// (or an agent branching recovery) gets one coarse `status` token to decide
// what to do next, instead of parsing raw lock fields or a binary ping.
//
// Copied from Unity Open MCP's mcp-server/src/tools/bridge-status.ts (copy
// fidelity), with intentional deltas documented at the bottom of this file.
//
// Route: **local** — resolved in-process by `handleLocalTool` (no bridge POST).
// The handler reads the instance lock, classifies it, fires one /ping probe
// through the installed live router, and composes the result with the pure
// `deriveBridgeStatus` mapper. It works whether or not the bridge is up: a dead
// / stopped bridge is a *successful* status read, never an error.
//
// Read-only, gate-free, never spawns the editor. The /ping fetch uses the
// bridge's standard 5s timeout; this tool takes no arguments.
//
// Operator vs agent usage:
//   - Operators / the Validation Suite reach this for manual bridge-offline
//     scenarios (confirm a toolbar stop, confirm readiness on restart).
//   - Agents should prefer `unreal_open_mcp_ping` for routine health. Use
//     `bridge_status` when branching recovery: it distinguishes a transient
//     reload (`unreachable` — retry) from a dead bridge (`dead_bridge` — read
//     the Output Log) from a stopped editor (`stopped` — reopen the editor).
export const bridgeStatus: Tool = {
  name: "unreal_open_mcp_bridge_status",
  description:
    "Operator / recovery-oriented bridge health snapshot. Read-only " +
    "(gate-free). Composes the instance-lock classifier " +
    "(instance-discovery.ts#classifyInstance) with one /ping probe and returns " +
    "a coarse `status` token: `running` (bridge connected, idle), `compiling` " +
    "(bridge connected, editor compiling / Live Coding), `stopped` (editor not " +
    "running OR bridge toggle off — no live listener), `unreachable` (editor " +
    "process alive but the listener did not respond — usually a transient Live " +
    "Coding / domain-reload window; retry shortly), or `dead_bridge` (editor " +
    "process alive but the bridge module failed to recompile, so /ping will " +
    "never recover — read the Output Log for the compile errors that killed " +
    "the bridge). Also surfaces a top-level `classification` field " +
    "(healthy | reloading | dead_bridge | gone) mirroring the instance lock, a " +
    "structured `recoveryHint` ({ tool, reason }) that is non-null only when " +
    "the status has a specific recovery tool (dead_bridge → " +
    "unreal_open_mcp_read_compile_errors — the offline editor-log surface that " +
    "works with the bridge assembly dead; null otherwise), a `ping` summary " +
    "(reachable + connected/compiling/" +
    "isPlaying/versions when the probe succeeded, reachable:false otherwise), " +
    "an `instance` summary (lock path + classification + a redacted lock when a " +
    "live lock was found), and a human-readable `nextStep`. The result is a " +
    "successful status read in every case — a stopped or dead bridge is NOT an " +
    "error. Route: local (resolved in-process; no bridge POST). " +
    "Agent usage: prefer unreal_open_mcp_ping for routine health; call " +
    "bridge_status when you need to branch recovery. This tool takes no " +
    "arguments.",
  inputSchema: {
    type: "object",
    properties: {},
    additionalProperties: false,
  },
};

// Intentional deltas from Unity Open MCP (mcp-server/src/tools/bridge-status.ts):
//   - Name `unreal_open_mcp_bridge_status` (Unity: `unity_open_mcp_bridge_status`).
//   - `dead_bridge` recovery hint points at `unreal_open_mcp_read_compile_errors`
//     (the offline editor-log reader shipped in P8.7 — the one channel that
//     works when the bridge module itself failed to compile). Unity points its
//     equivalent at the same-named tool; the Unreal twin mirrors Unity's log-tail
//     + structured-diagnostic shape, adapted to `<Project>/Saved/Logs/*.log` and
//     MSVC/clang parsing. See bridge-status-helpers.ts#bridgeStatusRecoveryHint.
//   - No cold-Safe-Mode process-scan branch. Unity detects a Unity editor
//     running for this project even without a lock (Unity Safe Mode) and reuses
//     the dead_bridge token. Unreal has no MCP-side editor-process scanner yet,
//     so a missing lock + a ping failure classifies as `stopped`. This narrows
//     when a process scan lands. See bridge-status-helpers.ts#deriveBridgeStatus.
//   - Compiling/Unreal wording references Live Coding / module reload rather
//     than Unity's domain reload / Safe Mode.
