import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P5.4 — discover UFunctions on a class. The Unreal analog of Unity's
// find-members, adapted to Unreal reflection (TFieldIterator<UFunction>).
//
// First-party reflection only — no ReflectorNet / McpPlugin. Overrides are
// de-duped most-derived-wins; `matched` vs `returned` report the honest counts
// so a large class does not silently truncate.
//
// Route: live (POST /tools/unreal_open_mcp_reflection_method_find). Read-only.
export const reflectionMethodFind: Tool = {
  name: "unreal_open_mcp_reflection_method_find",
  description:
    "List the UFunctions on a class with a signature + flag descriptor per " +
    "method. Read-only (gate-free). `class` resolves as a native class path " +
    "('/Script/Engine.Actor'), a Blueprint asset/generated-class path, or a " +
    "short type name ('Actor'). Optional `name` is a case-insensitive substring " +
    "filter; `limit` bounds the result (default 100, <=0 = all). Overrides are " +
    "de-duped most-derived-wins. Returns { class, matched, returned, truncated, " +
    "methods:[{ name, returnType, params:[{ name, type, out }], flags:[...], " +
    "callable }] } where flags include BlueprintCallable / BlueprintPure / " +
    "Static / Const / Native / Event / CallInEditor and `callable` signals " +
    "whether reflection_method_call would accept it. Error codes: " +
    "missing_parameter (class absent), class_not_found (class did not resolve), " +
    "invalid_parameter (malformed body). Use this before reflection_method_call " +
    "to find an invokable function + its param names.",
  inputSchema: {
    type: "object",
    required: ["class"],
    properties: {
      class: {
        type: "string",
        description:
          "The class to inspect — native class path, Blueprint path, or short " +
          "type name.",
      },
      name: {
        type: "string",
        description:
          "Case-insensitive substring matched against function names.",
      },
      limit: {
        type: "integer",
        default: 100,
        description:
          "Max methods returned (default 100). <=0 returns all matches; " +
          "`matched` reports the pre-limit total.",
      },
    },
    additionalProperties: false,
  },
};
