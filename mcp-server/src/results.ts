// Shared CallToolResult error factory.
//
// Copied from Unity Open MCP's mcp-server/src/results.ts (copy fidelity, P1.7).
// A single named-argument factory avoids the positional-argument ambiguity that
// bit the Unity codebase (the compressible-router copy had `message, code` while
// the others used `code, message`). Named args make the call sites self-
// documenting and swap-proof.
//
// Wire shape: when `detail` is omitted (or nullish), the result body is
// `{ error: { code, message } }` — exactly what downstream parsers (and tests
// asserting `body.error.code`) expect. When `detail` is supplied, it replaces
// the error envelope entirely (callers that build a richer custom body rely on
// this).

import type { CallToolResult } from "@modelcontextprotocol/sdk/types.js";

export interface ErrorResultInput {
  /** Stable machine-readable code (e.g. `bridge_offline`, `bridge_timeout`). */
  code: string;
  /** Human-readable explanation. */
  message: string;
  /**
   * Optional custom body. When provided (non-nullish), it replaces the default
   * `{ error: { code, message } }` envelope. Use this when a caller needs to
   * surface richer structured data (e.g. HTTP status + body from the bridge).
   */
  detail?: unknown;
}

export function makeErrorResult(input: ErrorResultInput): CallToolResult {
  const body = input.detail ?? { error: { code: input.code, message: input.message } };
  return {
    content: [{ type: "text", text: JSON.stringify(body) }],
    isError: true,
  };
}
