import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P4.3 — read-only material parameter inventory. Wraps
// UMaterialEditingLibrary::Get*ParameterNames + the value getters so an agent
// can enumerate a material's scalar/vector/texture parameters and current
// values before chaining into material_modify. `path` is a material or
// material-instance (path-or-name). Values are the instance override for a
// UMaterialInstanceConstant, or the base-material default for a UMaterial.
//
// The scalars/vectors/textures shapes match material_modify's input so
// get-data output chains directly into a modify call: scalars is
// name->number, vectors is name->{r,g,b,a}, textures is name->path (or null
// when unset).
//
// Optional `paths` (dot-separated field names) projects the result to those
// branches only — saves tokens when the caller needs just one field.
//
// Intentional deltas vs Unity's material_get_properties:
//   - Reports the typed scalar/vector/texture parameter surface (the Unreal
//     material-instance analog), not a raw shader property dump.
//   - A base material still reports each parameter's DEFAULT value (not null).
//   - Naming follows the roadmap `material_*` style.
//
// Route: live (POST /tools/unreal_open_mcp_material_get_data). Read-only — no
// gate.
export const materialGetData: Tool = {
  name: "unreal_open_mcp_material_get_data",
  description:
    "Read the parameter inventory of a material or material instance. " +
    "Read-only (gate-free). Returns { path, isInstance, parent?, scalars, " +
    "vectors, textures } where `scalars` is name->number, `vectors` is " +
    "name->{r,g,b,a}, and `textures` is name->path (null when unset) — the " +
    "instance override for a UMaterialInstanceConstant, or the base-material " +
    "default for a UMaterial. `parent` is present only for instances. The " +
    "parameter shapes match material_modify input so this output chains " +
    "directly into a modify call. `path` accepts either an object path " +
    "('/Game/Mat/MI_Foo.MI_Foo') or a package path ('/Game/Mat/MI_Foo'). " +
    "Optional `paths` (dot-separated field names like ['scalars','parent']) " +
    "projects the result to those branches only — saves tokens. Use before " +
    "material_modify to learn valid parameter names. Error codes: " +
    "missing_parameter (path absent), asset_not_found (no asset at path), " +
    "not_a_material (path is not a material/material-interface), " +
    "invalid_parameter (malformed body or non-array `paths`).",
  inputSchema: {
    type: "object",
    required: ["path"],
    properties: {
      path: {
        type: "string",
        description:
          "Material or material-instance path-or-name — an object path " +
          "('/Game/Mat/MI_Foo.MI_Foo') or a package path ('/Game/Mat/MI_Foo').",
      },
      paths: {
        type: "array",
        items: { type: "string" },
        description:
          "Optional scoped projection — dot-separated field names " +
          "(['scalars','parent']) to emit only those branches of the result " +
          "and save tokens. An absent branch is dropped silently. Must be an " +
          "array; a non-array value is rejected with invalid_parameter.",
      },
    },
    additionalProperties: false,
  },
};
