import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P4.3 — material instance create. Wraps
// UMaterialInstanceConstantFactoryNew + IAssetTools::CreateAsset so an agent
// can spin up a UMaterialInstanceConstant (MIC) from a parent material
// interface without composing AssetTools factory calls via reflection.
// `parent` is the parent material/material-interface (path-or-name);
// `destination` is the full package path of the new instance. The parent must
// exist and be a UMaterialInterface; the destination must NOT already exist
// (no silent overwrite) and must live under a writable content root.
//
// Mutating: runs the full gate path (checkpoint -> create -> validate ->
// delta); `paths_hint` should list the destination path (the new asset is
// what the gate validates).
//
// Intentional deltas vs Unity's material_create:
//   - Unity create takes a `shader_name` + writes a `.mat`; Unreal v1 creates
//     a UMaterialInstanceConstant from a parent material interface (shader
//     swap / material_set_shader deferred — not in the P4 roadmap).
//   - Path roots are `/Game/...`, not `Assets/`. Destination parent folder
//     must already exist (call asset_create_folder first).
//   - Naming follows the roadmap `material_*` style.
//
// Route: live (POST /tools/unreal_open_mcp_material_create). Mutating.
export const materialCreate: Tool = {
  name: "unreal_open_mcp_material_create",
  description:
    "Create a UMaterialInstanceConstant (material instance) from a parent " +
    "material. `parent` is the parent material/material-interface path-or-name " +
    "(e.g. '/Game/Mat/M_Base' or '/Game/Mat/M_Base.M_Base'); `destination` is " +
    "the full package path of the new instance (e.g. '/Game/Mat/MI_Foo'). The " +
    "parent must exist and be a material interface; the destination must NOT " +
    "already exist — a collision is a structured asset_already_exists error (no " +
    "silent overwrite). Destination parent folder must already exist (call " +
    "asset_create_folder first). Refuses /Engine, /Script, /Temp with " +
    "invalid_content_root. Mutating: runs the full gate path (checkpoint -> " +
    "create -> validate -> delta); `paths_hint` MUST list the destination path " +
    "(e.g. ['/Game/Mat/MI_Foo']) — there is no whole-project fallback, set " +
    "gate:\"off\" to bypass. Chain into material_modify to set parameters and " +
    "material_get_data to read the parameter inventory. Result shape: " +
    "{ path, parent }. Error codes: missing_parameter (no parent/destination), " +
    "parent_not_found (parent missing), not_a_material (parent is not a " +
    "material interface), invalid_path (destination not a valid package path), " +
    "invalid_content_root (engine root), asset_already_exists (destination " +
    "collision), execution_error (CreateAsset returned null).",
  inputSchema: {
    type: "object",
    required: ["parent", "destination", "paths_hint"],
    properties: {
      parent: {
        type: "string",
        description:
          "Parent material path-or-name — an object path " +
          "('/Game/Mat/M_Base.M_Base') or a package path ('/Game/Mat/M_Base'). " +
          "Must resolve to a UMaterialInterface (material or material instance).",
      },
      destination: {
        type: "string",
        description:
          "Destination package path for the new material instance, e.g. " +
          "'/Game/Mat/MI_Foo'. Must NOT already exist (collision is a " +
          "structured error). Parent folder must already exist.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — destination asset path(s) the mutation is scoped " +
          "to, fed to the gate as the checkpoint + validate hint. REQUIRED " +
          "for mutating tools (the gate refuses an empty hint with " +
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
