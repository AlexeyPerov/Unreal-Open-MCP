import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P7.2 — scaffold a new C++ class (header + cpp) into <Project>/Source/<module>/
// from parent-kind templates, mirroring the editor's "New C++ Class" wizard.
// `class_name` is the BARE name without the U/A/F prefix (the prefix is derived
// from the parent). `parent_class` selects one of the four supported templates:
// UObject (default) / Actor / ActorComponent / None (a plain non-UCLASS class).
// `module` defaults to the project's primary module and MUST already exist as a
// folder under Source/ — no Build.cs / module authoring in P7.2 (a later phase
// wires that). Refuses to overwrite existing header/cpp at the destination
// unless `force:true`; if the cpp write fails the just-written header is rolled
// back so a stranded half-scaffold never blocks a clean retry without force.
//
// Every path access is JAILED to <Project>/Source — `..`, absolute-outside, and
// NTFS ADS (`:`) escapes return `path_escapes_jail` and never write. The
// scaffold emits a neutral project-file header comment (NOT a third-party
// license banner); agents can overwrite it via source_update.
//
// Mutating: runs the full gate path (checkpoint -> create -> validate ->
// delta); `paths_hint` MUST list the destination scope (e.g. `["<module>/"]` or
// the destined header/cpp relative paths) — there is no whole-project fallback,
// set gate:"off" to bypass. NOTE: source_create_class does NOT auto-compile —
// an agent calls source_compile (P7.3) after the scaffold + any source_update
// edits.
//
// Route: live (POST /tools/unreal_open_mcp_source_create_class). Mutating.
export const sourceCreateClass: Tool = {
  name: "unreal_open_mcp_source_create_class",
  description:
    "Scaffold a new C++ class — header + cpp generated from parent-kind " +
    "templates into <Project>/Source/<module>/, mirroring the editor's 'New " +
    "C++ Class'. `class_name` is the BARE name WITHOUT the U/A/F prefix (the " +
    "prefix is derived from `parent_class`). `parent_class` selects one of: " +
    "UObject (default) -> `U` prefix + `UOBJECT_BODY`, Actor -> `A` prefix + " +
    "Actor include, ActorComponent -> `U` prefix + ActorComponent include, " +
    "None -> plain non-UCLASS `F` class. `module` defaults to the project's " +
    "primary module and MUST already exist as a folder under Source/ (no " +
    "Build.cs / module authoring here). Refuses to overwrite an existing " +
    "header/cpp at the destination unless `force:true`; a failed cpp write " +
    "rolls back the just-written header so a stranded half-scaffold never " +
    "blocks a clean retry without force. Emits the `<MODULE>_API` dllexport " +
    "macro. JAILED to <Project>/Source — traversal / absolute-outside / NTFS " +
    "ADS escapes return path_escapes_jail and never write. Does NOT compile — " +
    "call source_compile (P7.3) after the scaffold + any source_update edits. " +
    "Mutating: runs the full gate path (checkpoint -> create -> validate -> " +
    "delta); `paths_hint` MUST list the destination scope (e.g. ['<module>/'] " +
    "or the destined header/cpp relative paths) — there is no whole-project " +
    "fallback, set gate:\"off\" to bypass. Result shape: { class_name (with " +
    "derived prefix), module, parent_class (or '(none)'), header, cpp, " +
    "is_uclass }. Error codes: missing_parameter / invalid_parameter (bad " +
    "class_name / module identifier or unsupported parent_class) / " +
    "module_not_found / already_exists (set force:true to overwrite) / " +
    "path_escapes_jail / write_failed.",
  inputSchema: {
    type: "object",
    required: ["class_name", "paths_hint"],
    properties: {
      class_name: {
        type: "string",
        description:
          "Bare class name WITHOUT the U/A/F prefix (the prefix is derived " +
          "from `parent_class`). Must be a legal C++ identifier (letters/" +
          "digits/underscore, no leading digit). e.g. 'MyActor' (NOT " +
          "'AMyActor').",
      },
      parent_class: {
        enum: ["UObject", "Actor", "ActorComponent", "None"],
        default: "UObject",
        description:
          "Parent kind. UObject (default) -> U prefix + UCLASS + UObject " +
          "base; Actor -> A prefix + UCLASS + AActor base; ActorComponent -> " +
          "U prefix + UCLASS + UActorComponent base; None -> plain F class " +
          "(no UCLASS, no base). Both bare ('Actor') and prefixed ('AActor') " +
          "forms are accepted.",
      },
      module: {
        type: "string",
        description:
          "Target module folder under Source/ (must already exist — no Build.cs " +
          "authoring here). Defaults to the project's primary module. Must be a " +
          "legal C++ identifier.",
      },
      force: {
        type: "boolean",
        default: false,
        description:
          "Overwrite an existing header/cpp at the destination. Default false " +
          "(already_exists on collision). When true, both files are overwritten.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — the Source-relative destination path(s) the " +
          "scaffold is scoped to, fed to the gate as the checkpoint + validate " +
          "hint. e.g. ['MyModule/'] or ['MyModule/MyActor.h', " +
          "'MyModule/MyActor.cpp']. REQUIRED for mutating tools (the gate " +
          "refuses an empty hint with paths_hint_required; there is no whole-" +
          "project fallback). Set gate:\"off\" to bypass the gate and skip the " +
          "hint.",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint -> create -> " +
          "validate -> delta and hard-fails on new Errors; warn commits the " +
          "scaffold but surfaces new Errors as warnings; off skips the gate " +
          "entirely (paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};
