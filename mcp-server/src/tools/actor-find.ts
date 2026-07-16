import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P2.2 — read-only actor find. Two modes (mirrors Unity's gameobject-find):
//   (a) targeted lookup by `actor` ref (label → name → path) → single-actor
//       result. A targeted miss returns {ok:true, notFound:true, actors:[]} —
//       it is NOT an error, so an agent can branch on "no match" cleanly.
//   (b) list mode (omit `actor`) → enumerate the editor world with optional
//       filters, bounded by max_results. Each result carries label, name,
//       class, path, and transform so the agent can chain into later actor
//       tools without a second call.
//
// Intentional deltas vs Unity's gameobject-find:
//   - Addressing is a string `actor` ref (Unreal has no global int instance
//     id); label → name → path resolution lives in the bridge.
//   - Filters are `class` + `name_contains` (Unreal tags/layers defer to a
//     later phase). `class` matches the actor's class or a subclass.
//   - The transform is reported as {location, rotation{pitch,yaw,roll}, scale}
//     (Unreal conventions), not Unity's euler x/y/z.
//
// Route: live (POST /tools/unreal_open_mcp_actor_find). Read-only — no gate.
export const actorFind: Tool = {
  name: "unreal_open_mcp_actor_find",
  description:
    "Find actors in the current editor level. Read-only (gate-free). Two modes: " +
    "(a) targeted lookup by `actor` ref (label → name → path) — returns a " +
    "single-actor result; when nothing matches, returns {ok:true, " +
    "notFound:true, actors:[]} so you can branch on 'no match' without parsing " +
    "an error. (b) list mode (omit `actor`) — enumerates actors in the editor " +
    "world with optional `class` / `name_contains` filters, bounded by " +
    "max_results (default 25, hard cap 100); `truncated` reports how many " +
    "matches were clipped. Each ActorData entry carries label, name, class, " +
    "path, and transform (location, rotation{pitch,yaw,roll}, scale); targeted " +
    "results also include a short `components` array so you can chain into " +
    "modify/component tools without a second call. Prefer this over raw " +
    "invoke_method AActor::Get* — structured output + addressing parity with " +
    "the rest of the actor family.",
  inputSchema: {
    type: "object",
    properties: {
      actor: {
        type: "string",
        description:
          "Targeted mode: actor ref resolved label → name → path (case-sensitive " +
          "first, then case-insensitive). When set, returns a single-actor " +
          "result (empty with notFound=true when nothing matches). Omit for " +
          "list mode.",
      },
      class: {
        type: "string",
        description:
          "List mode: filter by AActor subclass. Accepts a soft class path " +
          "('/Script/Engine.PointLight', '/Game/BP/BP_Foo.BP_Foo_C'), an asset " +
          "path to a Blueprint, or a short native type name ('StaticMeshActor'). " +
          "Matches the class or any subclass.",
      },
      name_contains: {
        type: "string",
        description:
          "List mode: case-insensitive substring matched against actor labels.",
      },
      max_results: {
        type: "integer",
        default: 25,
        minimum: 1,
        description:
          "List mode: max actors returned (default 25, hard cap 100). The " +
          "count clipped by the cap is reported in 'truncated'.",
      },
    },
    additionalProperties: false,
  },
};
