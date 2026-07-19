import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P4.4 — asset import. Wraps IAssetTools::ImportAssetTasks so an agent can
// bring an external art/audio source file into `/Game/...` without composing
// brittle AssetTools/Interchange factory calls via reflection. `file` is an
// ABSOLUTE host filesystem path (must exist); `destination` is the content
// FOLDER the asset lands in; `name` optionally overrides the asset name
// (default: the source file's base name).
//
// The import is synchronous and GC-safe (the bridge roots the transient
// UAssetImportTask across the import call). `replace_existing` (default false)
// refuses a collision unless set; `save` (default false) writes the package to
// disk after import — the default leaves it dirty in-memory so the caller can
// batch a save (or let the gate delta capture it).
//
// Supported types: whatever the installed AssetTools/Interchange importers
// accept — textures (PNG, JPG, TGA, EXR, ...), static meshes (FBX, OBJ, glTF),
// and sounds (WAV) are the common cases. An extension with no registered
// importer returns import_failed (v1 does not expose an FBX advanced-options
// matrix or a full Interchange pipeline UI).
//
// Mutating: runs the full gate path (checkpoint -> import -> validate ->
// delta); `paths_hint` should list the destination folder / imported asset
// path so the gate scopes its checkpoint + validate to the new content.
//
// Intentional deltas vs Unity's asset import:
//   - `file` is an ABSOLUTE host OS path read into a content package; Unity
//     often works from files already under `Assets/` or a package refresh.
//     The path jail is one-directional — `file` is NEVER treated as a content
//     path, and the destination must be under a writable content root.
//   - `destination` is a `/Game/...` content folder, not `Assets/...`.
//   - Importer surface is AssetTools/Interchange, not Unity AssetDatabase
//     importers; v1 keeps options minimal.
//
// Route: live (POST /tools/unreal_open_mcp_asset_import). Mutating.
export const assetImport: Tool = {
  name: "unreal_open_mcp_asset_import",
  description:
    "Import an external source file into the Content Browser under `/Game/...`. " +
    "`file` is an ABSOLUTE host filesystem path that MUST exist on disk (it is " +
    "never treated as a content path); `destination` is the content FOLDER the " +
    "asset lands in (e.g. '/Game/McpTemp'); `name` optionally overrides the " +
    "asset name (default: the source file's base name). Supported types are " +
    "whatever the installed AssetTools/Interchange importers accept — textures " +
    "(PNG, JPG, TGA, EXR), static meshes (FBX, OBJ, glTF), and sounds (WAV) are " +
    "the common cases; an extension with no registered importer returns " +
    "import_failed. `replace_existing` (default false) refuses a collision with " +
    "asset_already_exists unless set; `save` (default false) writes the package " +
    "to disk after import (default leaves it dirty in-memory). Refuses /Engine, " +
    "/Script, /Temp destinations with invalid_content_root. The import is " +
    "synchronous and GC-safe. Mutating: runs the full gate path (checkpoint -> " +
    "import -> validate -> delta); `paths_hint` MUST list the destination folder " +
    "/ imported asset path — there is no whole-project fallback, set " +
    'gate:"off" to bypass. Large source files can be slow — honour timeout_ms. ' +
    "Result shape: { imported: string[], destination, saved, replace_existing }. " +
    "Error codes: missing_parameter (no file/destination), file_not_found " +
    "(source missing on disk), invalid_path (destination not a valid content " +
    "folder), invalid_content_root (engine root), asset_already_exists " +
    "(destination collision, pass replace_existing:true), import_failed " +
    "(unsupported file type or importer error).",
  inputSchema: {
    type: "object",
    required: ["file", "destination", "paths_hint"],
    properties: {
      file: {
        type: "string",
        description:
          "Absolute host filesystem path of the source file to import (e.g. " +
          "'/Users/me/art/rock.png' or 'C:\\\\art\\\\rock.fbx'). Must exist on " +
          "disk. Never interpreted as a content path.",
      },
      destination: {
        type: "string",
        description:
          "Destination content FOLDER for the imported asset, e.g. " +
          "'/Game/McpTemp'. Must be under a project / plugin content root " +
          "(not /Engine, /Script, /Temp).",
      },
      name: {
        type: "string",
        description:
          "Optional asset name override. Defaults to the source file's base " +
          "name (e.g. 'rock.png' -> 'rock').",
      },
      replace_existing: {
        type: "boolean",
        default: false,
        description:
          "Overwrite an existing asset at the destination. Default false — a " +
          "collision is a structured asset_already_exists error (no silent " +
          "overwrite).",
      },
      save: {
        type: "boolean",
        default: false,
        description:
          "Save the imported package to disk after import. Default false " +
          "leaves the package dirty in-memory.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — the destination folder / imported asset path(s) " +
          "fed to the gate as the checkpoint + validate hint. REQUIRED for " +
          "mutating tools (the gate refuses an empty hint with " +
          "paths_hint_required; there is no whole-project fallback). Set " +
          'gate:"off" to bypass the gate and skip the hint.',
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint -> import -> " +
          "validate -> delta and hard-fails on new Errors; warn commits the " +
          "import but surfaces new Errors as warnings; off skips the gate " +
          "entirely (paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};
