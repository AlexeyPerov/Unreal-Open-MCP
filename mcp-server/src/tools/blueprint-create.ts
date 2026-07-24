import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// Blueprint create. Wraps FKismetEditorUtilities::CreateBlueprint so an agent
// can spin up a new Blueprint class from a parent UClass without composing
// Kismet / AssetRegistry calls via reflection. `path` is the /Game package
// path for the new asset (e.g. '/Game/Mcp/BP_Thing'); the object-path form
// ('/Game/Mcp/BP_Thing.BP_Thing') is also accepted and normalised by the
// bridge. `parent_class` defaults to Actor ('/Script/Engine.Actor'); any
// Blueprintable parent is allowed when CanCreateBlueprintOfClass passes
// (native class path or short name). The asset is registered in-session
// (AssetRegistry.AssetCreated + MarkPackageDirty); no disk save — chain into
// the later structure-edit + compile tools, then save via the asset family.
//
// This is the Blueprint family SPINE: blueprint_get + every later Blueprint
// tool resolves the asset created here via the shared ResolveBlueprint helper
// (in-memory first, then load — session-created assets may be unsaved).
//
// Mutating: runs the full gate path (checkpoint -> create -> validate ->
// delta); `paths_hint` MUST list the new Blueprint package path (e.g.
// ['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set
// gate:"off" to bypass.
//
// Fidelity: greenfield. There is no Unity Blueprint / prefab-graph twin; the
// Unity-first protocol still applies to the shared infrastructure (gate
// contract, snake_case naming, MCP envelope). Behavior reference (read-only):
// Unreal-MCP's blueprint-create for the Kismet create API + the any-UObject
// collision probe.
//
// Intentional deltas vs Unreal-MCP:
//   - Canonical MCP envelope + gate summary on create (Unreal-MCP mutates
//     directly with no gate).
//   - snake_case arg names (`parent_class`); the bridge also accepts the
//     `parentClass` alias.
//   - Writable-root refuse (/Engine, /Script, /Temp) — Unreal-MCP has none.
//
// Route: live (POST /tools/unreal_open_mcp_blueprint_create). Mutating.
export const blueprintCreate: Tool = {
  name: "unreal_open_mcp_blueprint_create",
  description:
    "Create a new Blueprint class from a parent class. `path` is the /Game " +
    "package path for the new asset (e.g. '/Game/Mcp/BP_Thing'); the " +
    "object-path form ('/Game/Mcp/BP_Thing.BP_Thing') is also accepted and " +
    "normalised. `parent_class` defaults to Actor " +
    "('/Script/Engine.Actor'); any Blueprintable parent is allowed when " +
    "CanCreateBlueprintOfClass passes — a native class path or short name " +
    "(e.g. '/Script/Engine.Actor' or 'Actor'). The asset is registered " +
    "in-session (Content Browser shows a dirty, unsaved asset) — no disk " +
    "save; chain into the structure-edit + compile tools, then save via the " +
    "asset family. Destination parent folder must already exist (call " +
    "asset_create_folder first). Refuses /Engine, /Script, /Temp with " +
    "invalid_content_root. Mutating: runs the full gate path (checkpoint -> " +
    "create -> validate -> delta); `paths_hint` MUST list the new Blueprint " +
    "package path (e.g. ['/Game/Mcp/BP_Thing']) — there is no whole-project " +
    "fallback, set gate:\"off\" to bypass. Chain into blueprint_get to " +
    "inspect the summary. Result shape: { name, path, parentClass }. Error " +
    "codes: missing_parameter (no path), parent_class_not_found (parent did " +
    "not resolve), parent_not_blueprintable (CanCreateBlueprintOfClass " +
    "false), invalid_package_path (path is not a valid long package name), " +
    "invalid_content_root (engine root), asset_already_exists (any asset at " +
    "the target path — not just a Blueprint), create_failed (CreateBlueprint " +
    "returned null), invalid_parameter (malformed body).",
  inputSchema: {
    type: "object",
    required: ["path", "paths_hint"],
    properties: {
      path: {
        type: "string",
        description:
          "Package path for the new Blueprint asset, e.g. " +
          "'/Game/Mcp/BP_Thing'. The object-path form " +
          "('/Game/Mcp/BP_Thing.BP_Thing') is also accepted and normalised " +
          "to the package path. Parent folder must already exist.",
      },
      parent_class: {
        type: "string",
        default: "/Script/Engine.Actor",
        description:
          "Parent UClass path or short name, e.g. '/Script/Engine.Actor' " +
          "or 'Actor'. Defaults to Actor. Must be Blueprintable " +
          "(CanCreateBlueprintOfClass). The camelCase alias `parentClass` is " +
          "also accepted by the bridge.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — the new Blueprint package path(s) the mutation " +
          "is scoped to, fed to the gate as the checkpoint + validate hint. " +
          "REQUIRED for mutating tools (the gate refuses an empty hint with " +
          "paths_hint_required; there is no whole-project fallback). Set " +
          "gate:\"off\" to bypass the gate and skip the hint.",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint -> create -> " +
          "validate -> delta and hard-fails on new Errors; warn commits the " +
          "mutation but surfaces new Errors as warnings; off skips the gate " +
          "entirely (paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};
