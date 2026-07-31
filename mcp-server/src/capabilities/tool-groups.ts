// Canonical tool-group catalog.
//
// Single source of truth for the per-session tool-group visibility system.
// Drives two surfaces:
//
//  - `unreal_open_mcp_manage_tools` meta-tool (P8.10 — session activation state).
//  - `unreal_open_mcp_capabilities` `toolGroups` block (compiled/session-agnostic
//    catalog only; per-session activation state is NOT reported here).
//
// Every registered MCP tool maps to a group id via `groupFor(toolName)`. Tools
// with no entry map to `null` and are always visible — they are server
// meta-tools and the offline recovery surface (capabilities, ping,
// bridge_status, read_compile_errors, …). The session store
// (`tool-session-state.ts`) additionally carries an `ALWAYS_VISIBLE_TOOLS`
// allow-list that bypasses the filter regardless of group assignment, so an
// always-visible tool survives even when its group is torn down.
//
// Groups are stable lowercase identifiers. DEFAULT_ENABLED_GROUPS is derived
// from catalog entries whose `defaultEnabled` flag is true. The Unreal lean
// baseline is `core` only (the roadmap P8.9 pin) — every other group
// activates on demand via manage_tools (P8.10) or a future domain-pack
// auto-activate (P12).
//
// Adapted from Unity Open MCP's mcp-server/src/capabilities/tool-groups.ts
// (copy fidelity for the skeleton + catalog/groupFor contract). Intentional
// deltas from Unity:
//   1. Smaller catalog — only the groups the Unreal surface needs today
//      (core / gate-and-verify / typed-editor / diagnostics). No profiler /
//      hub / senses / nav / input / probuilder / … groups until those tools
//      exist.
//   2. No domain package auto-activation (no domainDefine / unityPackage /
//      autoActivate fields) — Unreal domain packs are P12. The interface stays
//      minimal; fields can be added when a compile-gated domain lands.
//   3. Default-on set is `core` only (Unity also defaults `gate-and-verify`).

/**
 * Catalog entry for one tool group.
 */
export interface ToolGroup {
  /** Stable lowercase group id (e.g. `"core"`, `"typed-editor"`). */
  id: string;
  /** Short human-readable description of what the group covers. */
  description: string;
  /**
   * True when the group is enabled by default for every fresh session. The
   * Unreal baseline is `core` only; every other group is opt-in.
   */
  defaultEnabled: boolean;
}

/**
 * Ordered catalog. Order is preserved in manage_tools / capabilities output so
 * consumers render a stable list.
 */
export const TOOL_GROUPS: ToolGroup[] = [
  {
    id: "core",
    description:
      "Essential connectivity + discovery entry points (ping). Always on by " +
      "default; the lean baseline a fresh session advertises before any group " +
      "activation.",
    defaultEnabled: true,
  },
  {
    id: "gate-and-verify",
    description:
      "Gate, checkpoint, delta, apply_fix — the verify surface. Off by " +
      "default; activate when running the explicit checkpoint → mutate → " +
      "delta gate workflow.",
    defaultEnabled: false,
  },
  {
    id: "typed-editor",
    description:
      "Typed editor surface: actors, levels, assets, materials, editor state, " +
      "selection, console, reflection, screenshots, Blueprints, source. Off by " +
      "default; activate to mutate the editor world, content, or project C++.",
    defaultEnabled: false,
  },
  {
    id: "diagnostics",
    description:
      "Reserved for profiler / per-frame diagnostic reads. Empty in the " +
      "current phase; reserved so future profiler tools land in a named group " +
      "without reshuffling the catalog.",
    defaultEnabled: false,
  },
];

/**
 * Set of group ids enabled by default for every fresh session. The roadmap
 * P8.9 pin is `core` only — a fresh ListTools advertises the `core` tools plus
 * the always-visible meta / recovery tools, nothing else.
 */
export const DEFAULT_ENABLED_GROUPS: ReadonlySet<string> = new Set(
  TOOL_GROUPS.filter((g) => g.defaultEnabled).map((g) => g.id),
);

/** All known group ids (validates activate / deactivate input). */
export const GROUP_IDS: ReadonlySet<string> = new Set(
  TOOL_GROUPS.map((g) => g.id),
);

const GROUP_BY_ID: ReadonlyMap<string, ToolGroup> = new Map(
  TOOL_GROUPS.map((g) => [g.id, g]),
);

export function getGroup(id: string): ToolGroup | undefined {
  return GROUP_BY_ID.get(id);
}

// ---------------------------------------------------------------------------
// Per-tool group assignment — the authoritative mapping from a registered MCP
// tool name to its group id. Tools not listed here default to `null` (always
// visible). These are the server meta-tools (capabilities) and the offline
// recovery surface (bridge_status, read_compile_errors, source_read_offline,
// project_index) — tools an agent must reach even when every group is torn
// down. The ALWAYS_VISIBLE_TOOLS allow-list in tool-session-state.ts is the
// belt-and-suspenders that makes "always visible" survive a core teardown too.
// ---------------------------------------------------------------------------

const TOOL_GROUP_ASSIGNMENT: Record<string, string> = {};

function assign(group: string, names: string[]): void {
  for (const name of names) TOOL_GROUP_ASSIGNMENT[name] = group;
}

// --- core -------------------------------------------------------------------
// ping is the connectivity probe. It is ALSO in the ALWAYS_VISIBLE_TOOLS
// allow-list so it survives a `core` teardown — an agent that just deactivated
// core still needs to re-probe the bridge before re-activating.
assign("core", ["unreal_open_mcp_ping"]);

// --- gate-and-verify --------------------------------------------------------
assign("gate-and-verify", [
  "unreal_open_mcp_validate_edit",
  "unreal_open_mcp_checkpoint_create",
  "unreal_open_mcp_delta",
  "unreal_open_mcp_apply_fix",
]);

// --- typed-editor -----------------------------------------------------------
// The full typed editor surface — every tool that mutates or inspects the
// editor world, content, or project C++. Grouped together so one activation
// lights up the whole authoring surface.
assign(
  "typed-editor",
  [
    // actor family
    "actor_find",
    "actor_create",
    "actor_modify",
    "object_modify",
    "actor_set_parent",
    "actor_duplicate",
    "actor_destroy",
    "actor_component_add",
    "actor_component_destroy",
    "actor_component_get",
    "actor_component_modify",
    "actor_component_list_all",
    // level family
    "level_open",
    "level_save",
    "level_list_loaded",
    "level_set_current",
    "level_unload_sublevel",
    "level_get_data",
    "level_create",
    // asset family
    "asset_find",
    "asset_get_data",
    "asset_create_folder",
    "asset_copy",
    "asset_move",
    "asset_delete",
    "asset_refresh",
    "asset_import",
    // material family
    "material_create",
    "material_modify",
    "material_get_data",
    // editor family
    "editor_application_get_state",
    "editor_application_set_state",
    "editor_selection_get",
    "editor_selection_set",
    // console family
    "console_get_logs",
    "console_clear_logs",
    "console_run_command",
    // reflection family
    "reflection_method_find",
    "reflection_method_call",
    // screenshot family
    "screenshot_viewport",
    "screenshot_game_view",
    "screenshot_camera",
    "screenshot_isolated",
    // blueprint family
    "blueprint_create",
    "blueprint_get",
    "blueprint_add_component",
    "blueprint_remove_component",
    "blueprint_add_variable",
    "blueprint_modify_variable",
    "blueprint_set_default",
    "blueprint_add_function",
    "blueprint_add_event",
    "blueprint_compile",
    "blueprint_spawn",
    // source family
    "source_read",
    "source_list",
    "source_create_class",
    "source_update",
    "source_delete",
    "source_compile",
  ].map((suffix) => `unreal_open_mcp_${suffix}`),
);

// --- diagnostics ------------------------------------------------------------
// Reserved / empty for the current phase. Profiler + per-frame diagnostic
// reads land here in a later phase. Kept in the catalog so the group id is
// stable for manage_tools / capabilities output before any tool maps to it.

// ---------------------------------------------------------------------------
// Read API
// ---------------------------------------------------------------------------

/**
 * Resolve a tool name to its group id. Returns `null` for tools with no
 * assignment (server meta-tools + the offline recovery surface: capabilities,
 * bridge_status, read_compile_errors, source_read_offline, project_index).
 * Null means "always visible" — manage_tools and ListTools never hide a
 * null-group tool.
 */
export function groupFor(toolName: string): string | null {
  return TOOL_GROUP_ASSIGNMENT[toolName] ?? null;
}

/**
 * Inverse map: group id → sorted tool names. Used by manage_tools
 * `list_groups` (P8.10) to enumerate every group with its tool roster, and by
 * the capabilities `toolGroups` block to report per-group tool counts. Tools
 * with a null group are intentionally omitted (they are always-visible meta /
 * recovery tools). Computed ONCE at module load and frozen.
 */
const GROUP_TO_TOOLS: Readonly<Record<string, readonly string[]>> = (() => {
  const out: Record<string, string[]> = {};
  for (const group of TOOL_GROUPS) {
    out[group.id] = [];
  }
  for (const [tool, group] of Object.entries(TOOL_GROUP_ASSIGNMENT)) {
    if (!out[group]) out[group] = [];
    out[group].push(tool);
  }
  for (const names of Object.values(out)) names.sort();
  return out;
})();

export function groupToTools(): Record<string, readonly string[]> {
  return GROUP_TO_TOOLS;
}
