import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P5.3 — read a filtered slice of the bridge's bounded GLog ring buffer. The
// Unreal analog of Unity's console-log / read-console, adapted to the Unreal
// GLog sink.
//
// The ring is a plugin-owned, session-scoped recent-N window over engine /
// UE_LOG output captured after the plugin started — NOT a guaranteed full
// Output Log mirror. It is distinct from the call-scoped `logs[]` some mutating
// tools return: that captures a single call; this is the session ring fed by
// the one GLog callback.
//
// Route: live (POST /tools/unreal_open_mcp_console_get_logs). Read-only.
export const consoleGetLogs: Tool = {
  name: "unreal_open_mcp_console_get_logs",
  description:
    "Read a filtered slice of the recent engine log ring buffer (UE_LOG / " +
    "engine output captured since the plugin started). Read-only (gate-free). " +
    "Filters: `verbosity` (minimum severity — fatal|error|warning|display|log|" +
    "verbose|veryverbose|all; keeps entries at least as severe), `category` " +
    "(exact, case-insensitive), `contains` (case-insensitive substring), " +
    "`limit` (default 200, hard cap 2000 — returns the most recent matches). " +
    "Returns { entries:[{ sequence, verbosity, category, message, timestamp }], " +
    "count, matched, truncated } where matched is the pre-limit total and " +
    "truncated is matched > count. Caveats: this is a bounded recent-N window " +
    "(oldest entries are overwritten), lines before plugin start are not " +
    "captured (cold-start gap), and very long messages are truncated. It is " +
    "distinct from the call-scoped logs[] some tools return. Error code: " +
    "invalid_parameter (malformed body or unknown verbosity).",
  inputSchema: {
    type: "object",
    properties: {
      verbosity: {
        enum: [
          "fatal",
          "error",
          "warning",
          "display",
          "log",
          "verbose",
          "veryverbose",
          "all",
        ],
        description:
          "Minimum severity (inclusive). 'warning' keeps fatal+error+warning; " +
          "'all' (default) keeps everything.",
      },
      category: {
        type: "string",
        description:
          "Log category exact match (case-insensitive), e.g. 'LogTemp', " +
          "'LogBlueprint'.",
      },
      contains: {
        type: "string",
        description: "Case-insensitive substring the message must contain.",
      },
      limit: {
        type: "integer",
        default: 200,
        minimum: 1,
        description:
          "Max entries returned — the most recent matches (default 200, hard " +
          "cap 2000).",
      },
    },
    additionalProperties: false,
  },
};
