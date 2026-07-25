// Shared CallToolResult error factory.
//
// Copied from Unity Open MCP's mcp-server/src/results.ts (copy fidelity, P1.7).
// A single named-argument factory avoids the positional-argument ambiguity that
// bit the Unity codebase (the compressible-router copy had `message, code` while
// the others used `code, message`). Named args make the call sites self-
// documenting and swap-proof.
//
// Wire shape: the body ALWAYS carries an `error: { code, message }` envelope —
// that is the invariant every downstream parser (and every test asserting
// `body.error.code`) depends on. When `detail` is supplied it is MERGED over
// that envelope rather than replacing it, so a caller can add sibling blocks
// (e.g. `gate`) or override `error` with the bridge's own richer version.
//
// `detail` used to replace the body outright. That silently dropped the caller's
// computed `code`/`message` whenever the supplied object had no `error` key —
// e.g. a non-bridge service squatting on the deterministic per-project port, a
// proxy's 502 HTML-ish JSON, or a bare string. The emitted text was then that
// foreign body with NO error.code at all, breaking every `body.error.code` read
// downstream.

import type { CallToolResult } from "@modelcontextprotocol/sdk/types.js";

export interface ErrorResultInput {
  /** Stable machine-readable code (e.g. `bridge_offline`, `bridge_timeout`). */
  code: string;
  /** Human-readable explanation. */
  message: string;
  /**
   * Optional extra body fields, merged over the default
   * `{ error: { code, message } }` envelope. Use this to add sibling blocks
   * (e.g. `gate`) or to substitute the bridge's own `error` object. A
   * non-object value is nested under a `detail` key instead of being spread, so
   * the `error` envelope always survives.
   */
  detail?: unknown;
}

/** True for a plain (spreadable) JSON object — not null, not an array. */
function isPlainObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

export function makeErrorResult(input: ErrorResultInput): CallToolResult {
  const envelope: Record<string, unknown> = {
    error: { code: input.code, message: input.message },
  };

  if (input.detail != null) {
    if (isPlainObject(input.detail)) {
      Object.assign(envelope, input.detail);
    } else {
      // Arrays / strings / numbers cannot be merged without clobbering the
      // envelope, so preserve them under an explicit key.
      envelope.detail = input.detail;
    }
  }

  return {
    content: [{ type: "text", text: JSON.stringify(envelope) }],
    isError: true,
  };
}
