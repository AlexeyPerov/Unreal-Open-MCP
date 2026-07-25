import test from "node:test";
import assert from "node:assert/strict";

import { makeErrorResult } from "./results.js";

/** Parse the single text content block back into an object. */
function bodyOf(result: ReturnType<typeof makeErrorResult>): Record<string, unknown> {
  const block = result.content[0] as { type: string; text: string };
  assert.equal(block.type, "text");
  return JSON.parse(block.text) as Record<string, unknown>;
}

test("makeErrorResult emits the standard error envelope and isError:true", () => {
  const result = makeErrorResult({
    code: "bridge_offline",
    message: "Bridge is not reachable.",
  });
  assert.equal(result.isError, true);
  assert.deepEqual(bodyOf(result), {
    error: { code: "bridge_offline", message: "Bridge is not reachable." },
  });
});

test("detail merges sibling blocks alongside error (gate rides at top level)", () => {
  const gate = { ran: true, outcome: "validate_scan_failed", gateFailed: true };
  const result = makeErrorResult({
    code: "execution_error",
    message: "scanner blew up",
    detail: { error: { code: "execution_error", message: "scanner blew up" }, gate },
  });
  const body = bodyOf(result) as {
    error: { code: string };
    gate: typeof gate;
  };
  assert.equal(body.error.code, "execution_error");
  assert.deepEqual(body.gate, gate);
});

// Regression: `detail` used to REPLACE the body outright, so a JSON body with no
// `error` key erased the computed code and every downstream `body.error.code`
// read broke. Reachable whenever something that is not the bridge answers on the
// deterministic per-project port, or a proxy returns its own error body.
test("detail without an error key does not erase the computed error envelope", () => {
  const result = makeErrorResult({
    code: "bridge_http_error",
    message: "Bridge returned HTTP 502.",
    detail: { message: "Bad gateway", upstream: "nginx" },
  });
  const body = bodyOf(result) as {
    error: { code: string };
    upstream: string;
  };
  assert.equal(body.error.code, "bridge_http_error");
  assert.equal(body.upstream, "nginx");
});

test("detail overriding error wins (the bridge's own richer error object)", () => {
  const result = makeErrorResult({
    code: "bridge_http_error",
    message: "fallback",
    detail: { error: { code: "tool_not_found", message: "no such tool" } },
  });
  const body = bodyOf(result) as { error: { code: string; message: string } };
  assert.equal(body.error.code, "tool_not_found");
  assert.equal(body.error.message, "no such tool");
});

test("a non-object detail is nested, never spread over the envelope", () => {
  for (const detail of ["a bare string", [1, 2, 3], 42] as const) {
    const result = makeErrorResult({
      code: "bridge_response_unparsable",
      message: "not JSON",
      detail,
    });
    const body = bodyOf(result) as { error: { code: string }; detail: unknown };
    assert.equal(
      body.error.code,
      "bridge_response_unparsable",
      `error envelope must survive detail=${JSON.stringify(detail)}`,
    );
    assert.deepEqual(body.detail, detail);
  }
});

test("nullish detail leaves the plain envelope untouched", () => {
  for (const detail of [undefined, null]) {
    const body = bodyOf(
      makeErrorResult({ code: "c", message: "m", detail }),
    );
    assert.deepEqual(body, { error: { code: "c", message: "m" } });
  }
});
