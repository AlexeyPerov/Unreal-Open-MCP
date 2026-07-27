// Capability-discovery builder.
//
// Aggregates the full capability surface (tools + verify rules + fixes) that
// `unreal_open_mcp_capabilities` returns. Every registered tool ships as
// `implemented: true`; planned rules (the verify-rule roadmap) are listed with
// `implemented: false` and guidance so agents get structured "not yet
// available" signals instead of discovering gaps by trial and error.
//
// Route: **local** — no bridge round-trip required. An agent can call
// `unreal_open_mcp_capabilities` with the editor down and still get an
// accurate rule/fix/tool inventory. This is the same local-first pattern
// Unity Open MCP uses (mcp-server/src/capabilities/build-capabilities.ts);
// the rule/fix contents are Unreal-specific.
//
// Pure transformation module: dependencies (registered tools, rule/fix
// catalogs) are passed in by the caller so this file has zero runtime
// cross-file imports and loads cleanly under `node --experimental-strip-types`.
//
// P3.8 scope (intentional deltas from Unity):
//   1. Smaller rule catalog — Unreal v1 codes only (broken_soft_reference /
//      missing_blueprint_parent / compile_error issue codes). No asmdef / Unity-only rules.
//   2. No tool-groups / lifecycle / cost-hints blocks. Those land with the
//      Phase 8 routing + session-visibility work (manage_tools / tool-session-
//      state). The capabilities surface reports accurate rule/fix data now;
//      the richer metadata layers stack on top later without breaking the
//      contract (callers read only what they need).
//   3. Local-first builder — same as Unity; no need for a bridge round-trip
//      to list rules.

import type { Tool } from "@modelcontextprotocol/sdk/types.js";
import type {
  RuleCapability,
  FixCapability,
  CapabilityStatus,
} from "./rule-catalog.js";

// ---------------------------------------------------------------------------
// Route metadata — mirrors the per-tool route classification the live client
// applies. Tools route live by default; the meta-tools that never hit the
// bridge (capabilities is the first) carry `route: "local"`.
// ---------------------------------------------------------------------------

export type RoutePolicy = "live" | "offline" | "local";

const LOCAL_TOOLS: ReadonlySet<string> = new Set([
  "unreal_open_mcp_capabilities",
  // P5.7 — bridge_status composes the lock classifier + one /ping probe
  // in-process; no bridge POST round-trip.
  "unreal_open_mcp_bridge_status",
]);

function routePolicyFor(toolName: string): RoutePolicy {
  if (LOCAL_TOOLS.has(toolName)) return "local";
  return "live";
}

// ---------------------------------------------------------------------------
// Tool categories — semantic grouping that does not leak milestone IDs
// ---------------------------------------------------------------------------

const TOOL_CATEGORY: Record<string, string> = {
  unreal_open_mcp_ping: "core",
  unreal_open_mcp_actor_find: "actor",
  unreal_open_mcp_actor_create: "actor",
  unreal_open_mcp_actor_modify: "actor",
  unreal_open_mcp_object_modify: "actor",
  unreal_open_mcp_actor_set_parent: "actor",
  unreal_open_mcp_actor_duplicate: "actor",
  unreal_open_mcp_actor_destroy: "actor",
  unreal_open_mcp_actor_component_add: "actor",
  unreal_open_mcp_actor_component_destroy: "actor",
  unreal_open_mcp_actor_component_get: "actor",
  unreal_open_mcp_actor_component_modify: "actor",
  unreal_open_mcp_actor_component_list_all: "actor",
  unreal_open_mcp_level_open: "level",
  unreal_open_mcp_level_save: "level",
  unreal_open_mcp_level_list_loaded: "level",
  unreal_open_mcp_level_set_current: "level",
  unreal_open_mcp_level_unload_sublevel: "level",
  unreal_open_mcp_level_get_data: "level",
  unreal_open_mcp_level_create: "level",
  // P4.1 — asset read family (asset_find / asset_get_data). Read-only
  // AssetRegistry queries; route live, gate-free.
  unreal_open_mcp_asset_find: "asset",
  unreal_open_mcp_asset_get_data: "asset",
  // P4.2 — Content Browser CRUD (asset_create_folder / asset_copy /
  // asset_move / asset_delete / asset_refresh). The four create/copy/move/
  // delete tools are mutating (route live, default gate Enforce,
  // paths_hint required); asset_refresh is read-only.
  unreal_open_mcp_asset_create_folder: "asset",
  unreal_open_mcp_asset_copy: "asset",
  unreal_open_mcp_asset_move: "asset",
  unreal_open_mcp_asset_delete: "asset",
  unreal_open_mcp_asset_refresh: "asset",
  // P4.4 — asset_import (source file → project content). Mutating (route live,
  // default gate Enforce, paths_hint required).
  unreal_open_mcp_asset_import: "asset",
  // P4.3 — material family (material_create / material_modify /
  // material_get_data). create + modify are mutating (route live, default gate
  // Enforce, paths_hint required); material_get_data is read-only.
  unreal_open_mcp_material_create: "material",
  unreal_open_mcp_material_modify: "material",
  unreal_open_mcp_material_get_data: "material",
  // P3.5/P3.6 — gate + verify surface. Read-only meta-tools participate in
  // the gate workflow but bypass GatePolicy.Execute (no recursion).
  unreal_open_mcp_validate_edit: "gate-and-verify",
  unreal_open_mcp_checkpoint_create: "gate-and-verify",
  unreal_open_mcp_delta: "gate-and-verify",
  // P3.7 — apply_fix. Mutating tool (default gate Enforce).
  unreal_open_mcp_apply_fix: "gate-and-verify",
  // P3.8 — capability discovery. Local-route, read-only.
  unreal_open_mcp_capabilities: "capability-discovery",
  // P5.1 — editor application state (PIE). get-state read-only; set-state
  // mutating (route live, default gate Enforce, paths_hint required).
  unreal_open_mcp_editor_application_get_state: "editor",
  unreal_open_mcp_editor_application_set_state: "editor",
  // P5.2 — editor selection. get read-only; set mutating (route live,
  // default gate Enforce, paths_hint required).
  unreal_open_mcp_editor_selection_get: "editor",
  unreal_open_mcp_editor_selection_set: "editor",
  // P5.3 — console family. get/clear read-only; run-command mutating (route
  // live, default gate Enforce, paths_hint required).
  unreal_open_mcp_console_get_logs: "console",
  unreal_open_mcp_console_clear_logs: "console",
  unreal_open_mcp_console_run_command: "console",
  // P5.4 — reflection family. method-find read-only; method-call mutating
  // (route live, default gate Enforce, paths_hint required).
  unreal_open_mcp_reflection_method_find: "reflection",
  unreal_open_mcp_reflection_method_call: "reflection",
  // P5.5 — screenshot family. Four read-only image-capture tools (route live,
  // gate-free; return a base64 PNG as MCP image content).
  unreal_open_mcp_screenshot_viewport: "screenshot",
  unreal_open_mcp_screenshot_game_view: "screenshot",
  unreal_open_mcp_screenshot_camera: "screenshot",
  unreal_open_mcp_screenshot_isolated: "screenshot",
  // P5.7 — bridge_status. Operator / recovery health snapshot. Local-route
  // (composes the lock classifier + one /ping probe in-process), read-only,
  // gate-free. Sits in the core meta bucket alongside ping / capabilities.
  unreal_open_mcp_bridge_status: "core",
  // P6.1 — Blueprint family (blueprint_create / blueprint_get). create is
  // mutating (route live, default gate Enforce, paths_hint required); get is
  // read-only.
  unreal_open_mcp_blueprint_create: "blueprint",
  unreal_open_mcp_blueprint_get: "blueprint",
  // P6.2 — Blueprint SCS components (blueprint_add_component /
  // blueprint_remove_component). Both mutating (route live, default gate
  // Enforce, paths_hint required) — add/remove nodes on the Blueprint's
  // Simple Construction Script.
  unreal_open_mcp_blueprint_add_component: "blueprint",
  unreal_open_mcp_blueprint_remove_component: "blueprint",
  // P6.3 — Blueprint member variables + CDO defaults. All mutating (route live,
  // default gate Enforce, paths_hint required).
  unreal_open_mcp_blueprint_add_variable: "blueprint",
  unreal_open_mcp_blueprint_modify_variable: "blueprint",
  unreal_open_mcp_blueprint_set_default: "blueprint",
  // P6.4 — Blueprint function / event graph stubs. Both mutating (route live,
  // default gate Enforce, paths_hint required) — add_function creates an empty
  // user function-graph stub; add_event enables or creates an overridable
  // parent event node. Body authoring is out of scope (stub-only).
  unreal_open_mcp_blueprint_add_function: "blueprint",
  unreal_open_mcp_blueprint_add_event: "blueprint",
  // P6.5 — Blueprint compile. Mutating (route live, default gate Enforce,
  // paths_hint required) — compiles a Blueprint and returns a structured
  // error/warning list. A failed compile is a normal result (ok:true +
  // succeeded:false), not a transport failure.
  unreal_open_mcp_blueprint_compile: "blueprint",
  // P6.6 — Blueprint spawn. Mutating (route live, default gate Enforce,
  // paths_hint required) — instances a compiled Actor Blueprint's
  // GeneratedClass into the current editor level via UWorld::SpawnActor
  // (headless-safe). Closes the create -> edit -> compile -> spawn loop.
  unreal_open_mcp_blueprint_spawn: "blueprint",
  // P7.1 — Source read/list family. Two read-only tools that inspect project
  // C++ under <Project>/Source/ via the shared bridge jail helpers. Both JAILED
  // to Source/ (escapes never read); no gate path (read-only).
  unreal_open_mcp_source_read: "source",
  unreal_open_mcp_source_list: "source",
  // P7.2 — Source CRUD mutators. Three mutating tools (route live, default gate
  // Enforce, paths_hint required) that scaffold / edit / remove source files
  // under <Project>/Source/ via the P7.1 jail helpers. create_class scaffolds a
  // header + cpp from parent-kind templates; update is full-file replace or
  // line-range splice; delete removes a single file. All three JAILED to
  // Source/ (escapes never write); no compile here (P7.3 lands it).
  unreal_open_mcp_source_create_class: "source",
  unreal_open_mcp_source_update: "source",
  unreal_open_mcp_source_delete: "source",
};

function categoryFor(toolName: string): string {
  return TOOL_CATEGORY[toolName] ?? "other";
}

// ---------------------------------------------------------------------------
// Capability descriptors
// ---------------------------------------------------------------------------

export interface ToolCapability {
  name: string;
  implemented: boolean;
  status: CapabilityStatus;
  category: string;
  description: string;
  routePolicy: RoutePolicy;
  /** Input schema mirrored from the Tool definition. */
  inputSchema: Tool["inputSchema"];
}

export interface CapabilitiesResult {
  tools: ToolCapability[];
  rules: RuleCapability[];
  fixes: FixCapability[];
  counts: {
    toolsImplemented: number;
    rulesImplemented: number;
    rulesPlanned: number;
    fixesImplemented: number;
  };
}

export interface CapabilitiesFilter {
  /** Filter to a single surface (`tools` | `rules` | `fixes`). Omit for all. */
  kind?: "tools" | "rules" | "fixes";
  /** When false, omit planned/unimplemented capabilities. */
  includePlanned?: boolean;
}

// ---------------------------------------------------------------------------
// Dependencies — injected by the caller so this module stays import-free
// ---------------------------------------------------------------------------

export interface BuildCapabilitiesDeps {
  tools: Tool[];
  rules: RuleCapability[];
  fixes: FixCapability[];
}

export function buildCapabilities(
  deps: BuildCapabilitiesDeps,
  filter: CapabilitiesFilter = {},
): CapabilitiesResult {
  const includePlanned = filter.includePlanned !== false;

  const tools: ToolCapability[] = deps.tools.map((tool) => ({
    name: tool.name,
    implemented: true,
    status: "implemented",
    category: categoryFor(tool.name),
    description: tool.description ?? "",
    routePolicy: routePolicyFor(tool.name),
    inputSchema: tool.inputSchema,
  }));

  const rules = includePlanned
    ? deps.rules
    : deps.rules.filter((r) => r.implemented);

  const fixes = includePlanned
    ? deps.fixes
    : deps.fixes.filter((f) => f.implemented);

  return {
    tools: filter.kind === "rules" || filter.kind === "fixes" ? [] : tools,
    rules: filter.kind === "tools" || filter.kind === "fixes" ? [] : rules,
    fixes: filter.kind === "tools" || filter.kind === "rules" ? [] : fixes,
    counts: {
      toolsImplemented: tools.length,
      rulesImplemented: deps.rules.filter((r) => r.implemented).length,
      rulesPlanned: includePlanned
        ? deps.rules.filter((r) => !r.implemented).length
        : 0,
      fixesImplemented: deps.fixes.filter((f) => f.implemented).length,
    },
  };
}
