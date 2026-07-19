import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P4.3 — material instance parameter edits. Wraps
// UMaterialEditingLibrary::SetMaterialInstance{Scalar,Vector,Texture}
// ParameterValue so an agent can override parameters on a
// UMaterialInstanceConstant. `path` is the instance (path-or-name); at least
// one of `scalars` (name -> number), `vectors` (name -> {r,g,b,a}), or
// `textures` (name -> texture path-or-name) MUST be supplied — an empty no-op
// is refused with nothing_to_modify. Changes are in-memory (the package is
// marked dirty); pass `save: true` to also write the package to disk.
//
// Because the engine parameter setters silently ignore unknown names,
// applied-vs-failed is determined by validating each requested name against
// the instance's known parameter set: unknown names / missing textures land in
// the `failed` list rather than aborting the call. When NOTHING applies, the
// call reports nothing_to_modify and leaves the package untouched. Read the
// parameter inventory with material_get_data first to avoid typos.
//
// Mutating: runs the full gate path (checkpoint -> modify -> validate ->
// delta); `paths_hint` should list the instance path.
//
// Intentional deltas vs Unity's material_set_property:
//   - Batch scalars/vectors/textures maps replace Unity's single-property set
//     (an agent can set many params per call).
//   - Vector params take an {r,g,b,a} object; a partial object (e.g. {"r":1})
//     preserves the other components (seeded from the current value).
//   - Naming follows the roadmap `material_*` style.
//
// Route: live (POST /tools/unreal_open_mcp_material_modify). Mutating.
export const materialModify: Tool = {
  name: "unreal_open_mcp_material_modify",
  description:
    "Set parameters on a UMaterialInstanceConstant. `path` is the instance " +
    "path-or-name. Provide at least one of `scalars` (object of name->number), " +
    "`vectors` (object of name->{r,g,b,a}), or `textures` (object of " +
    "name->texture path-or-name) — an empty modify is refused with " +
    "nothing_to_modify. Unknown parameter names and missing textures are " +
    "collected in the `failed` list (they do not abort the call); if NOTHING " +
    "applies the call reports nothing_to_modify and leaves the package " +
    "untouched. A partial vector (e.g. {\"r\":1}) preserves the other " +
    "components. Changes are in-memory (package marked dirty); pass " +
    "`save`:true to also write the package to disk (default false). Read " +
    "material_get_data first to learn valid parameter names. Refuses /Engine, " +
    "/Script, /Temp with invalid_content_root. Mutating: runs the full gate " +
    "path (checkpoint -> modify -> validate -> delta); `paths_hint` MUST list " +
    "the instance path — there is no whole-project fallback, set gate:\"off\" " +
    "to bypass. Result shape: { path, applied: { scalars[], vectors[], " +
    "textures[] }, failed[], saved }. Error codes: missing_parameter (no " +
    "path), asset_not_found (path missing), invalid_content_root (engine " +
    "root), not_a_material_instance (path is not a UMaterialInstanceConstant), " +
    "nothing_to_modify (no params supplied, or none applied), invalid_parameter " +
    "(malformed body).",
  inputSchema: {
    type: "object",
    required: ["path", "paths_hint"],
    properties: {
      path: {
        type: "string",
        description:
          "Material-instance path-or-name — an object path " +
          "('/Game/Mat/MI_Foo.MI_Foo') or a package path ('/Game/Mat/MI_Foo'). " +
          "Must resolve to a UMaterialInstanceConstant.",
      },
      scalars: {
        type: "object",
        additionalProperties: { type: "number" },
        description:
          "Scalar parameter overrides — an object mapping parameter name to a " +
          "number, e.g. { \"Roughness\": 0.5 }. Unknown names land in `failed`.",
      },
      vectors: {
        type: "object",
        additionalProperties: {
          type: "object",
          properties: {
            r: { type: "number" },
            g: { type: "number" },
            b: { type: "number" },
            a: { type: "number" },
          },
        },
        description:
          "Vector/color parameter overrides — an object mapping parameter name " +
          "to an {r,g,b,a} object, e.g. { \"BaseColor\": { \"r\": 1, \"g\": 0, " +
          "\"b\": 0, \"a\": 1 } }. A partial object preserves the other " +
          "components (seeded from the current value).",
      },
      textures: {
        type: "object",
        additionalProperties: { type: "string" },
        description:
          "Texture parameter overrides — an object mapping parameter name to a " +
          "texture path-or-name, e.g. { \"Albedo\": \"/Game/Tex/T_Foo\" }. The " +
          "texture must exist; a missing texture lands in `failed`.",
      },
      save: {
        type: "boolean",
        default: false,
        description:
          "When true, write the package to disk after applying (in addition " +
          "to marking it dirty). Default false — the change stays in-memory " +
          "until the editor / a separate save persists it.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — the instance path(s) the mutation is scoped to, " +
          "fed to the gate as the checkpoint + validate hint. REQUIRED for " +
          "mutating tools (the gate refuses an empty hint with " +
          "paths_hint_required; there is no whole-project fallback). Set " +
          "gate:\"off\" to bypass the gate and skip the hint.",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint -> modify -> " +
          "validate -> delta and hard-fails on new Errors; warn commits the " +
          "mutation but surfaces new Errors as warnings; off skips the gate " +
          "entirely (paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};
