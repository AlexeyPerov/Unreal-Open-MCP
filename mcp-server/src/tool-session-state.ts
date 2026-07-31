// Per-session tool-group visibility state.
//
// Pure in-memory store: ephemeral, per connected MCP client/session. The MCP
// server is the authority for session visibility; the bridge does NOT track
// session state. Every MCP-server restart restores the catalog's default-on
// groups. One stdio server process has one connected client and one store.
//
// `unreal_open_mcp_manage_tools` (P8.10) is the only mutator of the tool-group
// state. ListTools reads it via `filterVisibleTools` to drop tools whose group
// is not active. In P8.9 only the store + filter are wired; manage_tools and
// the `notifications/tools/list_changed` signal land in P8.10. Unit tests
// exercise the store's `activate` / `deactivate` / `reset` directly.
//
// The store is intentionally not keyed by session id — the stdio MCP server
// has exactly one client per process. HTTP/SSE MCP transports would need a
// per-client map.
//
// Adapted from Unity Open MCP's mcp-server/src/tool-session-state.ts (copy
// fidelity for the ToolSessionState + filterVisibleTools contract). Intentional
// deltas from Unity:
//   1. No auto-activation machinery (no activateAuto / reconcileAutoActivation /
//      AUTO_ACTIVATE_GROUPS / ActivationSource "auto") — Unreal domain packs
//      are P12; no package-detection auto-activation in P8.
//   2. No "suppressed" ActivationSource — without auto-activation there is no
//      reconcile pass to guard against, so deactivation is a plain delete.
//      The simpler model keeps activate/deactivate symmetric and reset a clean
//      restore. Suppression semantics can be re-introduced when auto-activate
//      lands.
//   3. No fd-sample ring (`resource_pressure` is a Unity-only extra).
//   4. ALWAYS_VISIBLE_TOOLS covers the Unreal meta + offline recovery surface.

import type { Tool } from "@modelcontextprotocol/sdk/types.js";
import {
  DEFAULT_ENABLED_GROUPS,
  GROUP_IDS,
  getGroup,
  groupFor,
} from "./capabilities/tool-groups.js";

/**
 * Why a group is active in the current session.
 * - `"default"` — default-on group (in {@link DEFAULT_ENABLED_GROUPS}).
 * - `"manual"`  — activated via `unreal_open_mcp_manage_tools(action=activate)`
 *                 (P8.10). In P8.9 only unit tests drive this via the store API.
 */
export type ActivationSource = "default" | "manual";

/**
 * Names of always-visible tools. These are never filtered by the session
 * state — an agent can always reach them, even when every group (including
 * `core`) is torn down. Two kinds live here:
 *
 *  1. Server meta-tools — capabilities, manage_tools (once registered),
 *     read_compile_errors. They have no group assignment (groupFor → null),
 *     so the filter's null check would already keep them visible; listing
 *     them here is belt-and-suspenders and documents intent.
 *  2. Connectivity + recovery tools that DO carry a group assignment but must
 *     survive a teardown of that group:
 *     - `ping` (core group) — the precise connectivity probe an agent needs to
 *       re-probe the bridge after tearing down core before re-activating.
 *     - `bridge_status` — the operator recovery snapshot; sits in the core
 *       meta bucket and must survive a core teardown.
 *
 * The always-visible check runs FIRST in {@link filterVisibleTools}, so the
 * group assignment is a fallback that never applies for these tools.
 */
const ALWAYS_VISIBLE_TOOLS: ReadonlySet<string> = new Set([
  "unreal_open_mcp_capabilities",
  // manage_tools is not registered yet (P8.10); listed now so the allow-list
  // is complete the moment it lands and the filter needs no edit.
  "unreal_open_mcp_manage_tools",
  "unreal_open_mcp_ping",
  "unreal_open_mcp_bridge_status",
  "unreal_open_mcp_read_compile_errors",
]);

/**
 * Per-session tool-group visibility store.
 *
 * Lifecycle:
 *  - Constructed once per stdio server process (one connected MCP client).
 *  - Initial active set is {@link DEFAULT_ENABLED_GROUPS} — the groups marked
 *    `defaultEnabled: true` in the canonical tool-group catalog (see
 *    `capabilities/tool-groups.ts`). The Unreal lean baseline is `core` only;
 *    every other group activates on demand via manage_tools (P8.10).
 *  - Mutated only by {@link activate} / {@link deactivate} / {@link reset}
 *    (called from the manage_tools router in P8.10; unit tests in P8.9).
 *  - Read by {@link isGroupActive} (manage_tools list_groups, P8.10) and
 *    {@link filterVisibleTools} (the ListTools handler in index.ts).
 */
export class ToolSessionState {
  private active = new Set<string>(DEFAULT_ENABLED_GROUPS);
  /**
   * Per-group source tracking. Active groups carry `"default"` / `"manual"`.
   * A group that was never touched this session (initial opt-in state) has no
   * entry; {@link activationSource} returns `null` for those.
   */
  private source = new Map<string, ActivationSource>();

  constructor() {
    for (const id of DEFAULT_ENABLED_GROUPS) this.source.set(id, "default");
  }

  /** Snapshot of currently-active group ids (sorted for stable output). */
  activeGroups(): string[] {
    return Array.from(this.active).sort();
  }

  /** True when the group is in the active set. */
  isGroupActive(groupId: string): boolean {
    return this.active.has(groupId);
  }

  /**
   * Why the group is in its current state, or `null` when the group has never
   * been touched this session (initial opt-in state). Callers that only care
   * about "is it active" should use {@link isGroupActive}; this exposes the WHY
   * (default vs manual) for manage_tools reporting.
   */
  activationSource(groupId: string): ActivationSource | null {
    return this.source.get(groupId) ?? null;
  }

  /**
   * Activate a group. Returns true if state changed (group was not active).
   * Unknown groups are rejected with `false` — callers should validate via
   * {@link GROUP_IDS} first and surface a structured error.
   */
  activate(groupId: string): boolean {
    if (!GROUP_IDS.has(groupId)) return false;
    if (this.active.has(groupId)) return false;
    this.active.add(groupId);
    this.source.set(groupId, "manual");
    return true;
  }

  /**
   * Deactivate a group. Returns true if state changed (group was active).
   * Unknown groups are rejected with `false`. Deactivating the `core` group is
   * allowed — the meta-tools + ping + bridge_status stay reachable via
   * {@link ALWAYS_VISIBLE_TOOLS}, but the rest of the core surface goes dark
   * until the session re-activates it.
   */
  deactivate(groupId: string): boolean {
    if (!GROUP_IDS.has(groupId)) return false;
    if (!this.active.has(groupId)) return false;
    this.active.delete(groupId);
    this.source.delete(groupId);
    return true;
  }

  /**
   * Restore the default active set (see {@link DEFAULT_ENABLED_GROUPS}). Always
   * returns true. Called by manage_tools `reset` (P8.10) and implicitly on
   * every MCP-server restart (a fresh process builds a fresh store).
   */
  reset(): boolean {
    this.active = new Set(DEFAULT_ENABLED_GROUPS);
    this.source = new Map();
    for (const id of DEFAULT_ENABLED_GROUPS) this.source.set(id, "default");
    return true;
  }
}

/**
 * Filter a tool list to the tools visible in the current session.
 *
 * Visibility rules (precedence high → low):
 *  1. The tool name is in {@link ALWAYS_VISIBLE_TOOLS} → always visible.
 *  2. The tool has no group assignment (`groupFor` returns null) → always
 *     visible (defensive — matches the catalog intent for meta / recovery
 *     tools).
 *  3. The tool's group is in the session's active set → visible.
 *  4. Otherwise → hidden.
 *
 * `resolveGroup` is plumbed in so tests can swap the resolver; production
 * callers omit it and get the default catalog resolver.
 */
export function filterVisibleTools(
  tools: Tool[],
  state: ToolSessionState,
  resolveGroup: (toolName: string) => string | null = groupFor,
): Tool[] {
  return tools.filter((tool) => {
    if (ALWAYS_VISIBLE_TOOLS.has(tool.name)) return true;
    const group = resolveGroup(tool.name);
    if (group === null) return true;
    return state.isGroupActive(group);
  });
}

// Re-exported so the manage_tools router (P8.10) and the ListTools handler
// share one import surface. Keep the catalog definitions out of the public
// name — they are owned by tool-groups.ts.
export { getGroup, groupFor };
