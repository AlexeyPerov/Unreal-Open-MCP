import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P5.4 — invoke a UFunction (safety-gated ProcessEvent). The Unreal analog of
// Unity's invoke-method, adapted to Unreal reflection.
//
// Invoke on a live `target` (instance ref) XOR a `class` (the class default
// object). SAFETY: only BlueprintCallable or CallInEditor functions may be
// invoked; the CDO path additionally requires the function be static or
// CallInEditor. A non-callable function is rejected with method_not_callable
// BEFORE any invoke — this is not an arbitrary-ProcessEvent surface.
//
// Accepted risk: a BlueprintCallable / CallInEditor function can still be
// destructive (same class as console_run_command). v1 ships no per-function
// denylist; the flag allow-list + the mandatory gate (paths_hint) are the
// safeguards.
//
// Route: live (POST /tools/unreal_open_mcp_reflection_method_call). Mutating.
export const reflectionMethodCall: Tool = {
  name: "unreal_open_mcp_reflection_method_call",
  description:
    "Invoke a UFunction by name on a live `target` (instance ref) XOR a `class` " +
    "(the class default object). SAFETY GATE: only BlueprintCallable or " +
    "CallInEditor functions are invokable; the CDO (`class`) path additionally " +
    "requires a static or CallInEditor function — a non-callable function is " +
    "refused with method_not_callable before any invoke. `args` is an object " +
    "map of param-name → value (converted via the FProperty ⇄ JSON codec). " +
    "Returns { method, target?|class?, returnValue?, outs? } with the return " +
    "value and any out/by-ref params. WARNING: an allowed function can still be " +
    "destructive — same accepted-risk class as console_run_command (no " +
    "per-function denylist in v1). Mutating: `paths_hint` is required (the gate " +
    "refuses an empty hint with paths_hint_required; set gate:\"off\" to " +
    "bypass). Error codes: missing_parameter (method absent, or neither target " +
    "nor class), ambiguous_target (both target and class), target_not_found, " +
    "class_not_found, method_not_found, method_not_callable, invalid_argument " +
    "(an arg failed to convert). Use reflection_method_find first to find an " +
    "invokable function + its params.",
  inputSchema: {
    type: "object",
    required: ["method"],
    properties: {
      method: {
        type: "string",
        description: "The UFunction name to invoke.",
      },
      target: {
        type: "string",
        description:
          "Instance ref (actor/object, resolved label → name → path, or an " +
          "object path). Mutually exclusive with `class`.",
      },
      class: {
        type: "string",
        description:
          "Class ref — invokes on the class default object (CDO). Requires the " +
          "function be static or CallInEditor. Mutually exclusive with " +
          "`target`.",
      },
      args: {
        type: "object",
        description:
          "Param-name → value map. Missing params keep their default; each " +
          "supplied value is converted to its parameter type (invalid_argument " +
          "on mismatch).",
        additionalProperties: true,
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — the path(s) the invoke is scoped to, fed to the " +
          "gate as the checkpoint + validate hint. REQUIRED for this mutating " +
          "tool (the gate refuses an empty hint with paths_hint_required; there " +
          "is no whole-project fallback). Set gate:\"off\" to bypass.",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint → mutate → validate → " +
          "delta and hard-fails on new Errors; warn commits but surfaces new " +
          "Errors as warnings; off skips the gate entirely (paths_hint " +
          "optional). Precedence: request gate → tool default (enforce).",
      },
    },
    additionalProperties: false,
  },
};
