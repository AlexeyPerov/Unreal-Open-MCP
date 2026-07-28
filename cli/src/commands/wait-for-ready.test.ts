// Tests for the `wait-for-ready` command layer (commands/wait-for-ready.ts).
// The poll loop itself is covered by lib/ping-poller.test.ts (with injected
// fetch / clock / sleep); these tests cover the command-level resolution +
// formatting that sits on top of it.
//
// Built + run via the package test config (see package.json `test`).

import { test } from "node:test";
import assert from "node:assert/strict";

import {
  resolveProjectDir,
  formatHuman,
  formatJson,
  EXIT_OK,
  EXIT_TIMEOUT,
} from "./wait-for-ready.js";
import { PROJECT_PATH_ENV_VAR } from "../constants.js";
import type { PollOutcome } from "../lib/ping-poller.js";

// ---------------------------------------------------------------------------
// resolveProjectDir
// ---------------------------------------------------------------------------

test("resolveProjectDir: positional wins over --project / env / cwd", () => {
  assert.equal(
    resolveProjectDir({
      positionalProjectDir: "/pos",
      projectPath: "/flag",
      env: { [PROJECT_PATH_ENV_VAR]: "/env" },
      cwd: "/cwd",
    }),
    "/pos",
  );
});

test("resolveProjectDir: --project wins over env / cwd", () => {
  assert.equal(
    resolveProjectDir({
      projectPath: "/flag",
      env: { [PROJECT_PATH_ENV_VAR]: "/env" },
      cwd: "/cwd",
    }),
    "/flag",
  );
});

test("resolveProjectDir: env wins over cwd", () => {
  assert.equal(
    resolveProjectDir({
      env: { [PROJECT_PATH_ENV_VAR]: "/env" },
      cwd: "/cwd",
    }),
    "/env",
  );
});

test("resolveProjectDir: cwd fallback", () => {
  assert.equal(resolveProjectDir({ env: {}, cwd: "/cwd" }), "/cwd");
});

test("resolveProjectDir: relative positional is absolutized against cwd", () => {
  assert.equal(
    resolveProjectDir({
      positionalProjectDir: "relative/path",
      env: {},
      cwd: "/cwd",
    }),
    "/cwd/relative/path",
  );
});

test("resolveProjectDir: absolute positional is preserved as-is", () => {
  assert.equal(
    resolveProjectDir({
      positionalProjectDir: "/abs/path",
      env: {},
      cwd: "/cwd",
    }),
    "/abs/path",
  );
});

// ---------------------------------------------------------------------------
// formatHuman / formatJson
// ---------------------------------------------------------------------------

function readyOutcome(overrides: Partial<PollOutcome> = {}): PollOutcome {
  return {
    ready: true,
    status: "ready",
    lastPing: { connected: true, compiling: false },
    reason: "Bridge is ready (connected, idle).",
    elapsedMs: 1234,
    attempts: 3,
    port: 22028,
    url: "http://127.0.0.1:22028/ping",
    ...overrides,
  };
}

test("formatHuman: ready outcome prints elapsed + attempts + url", () => {
  const txt = formatHuman(readyOutcome(), "unreal-open-mcp-cli");
  assert.match(txt, /Ready after 1234ms/);
  assert.match(txt, /3 attempt/);
  assert.match(txt, /http:\/\/127\.0\.0\.1:22028\/ping/);
});

test("formatHuman: non-ready outcome prints the reason + binName", () => {
  const txt = formatHuman(
    readyOutcome({
      ready: false,
      status: "timeout",
      reason: "Bridge never became reachable within 5s.",
    }),
    "unreal-open-mcp-cli",
  );
  assert.match(txt, /unreal-open-mcp-cli: not ready/);
  assert.match(txt, /never became reachable/);
});

test("formatJson: outcome round-trips with ready discriminant", () => {
  const parsed = JSON.parse(formatJson(readyOutcome()));
  assert.equal(parsed.ready, true);
  assert.equal(parsed.status, "ready");
  assert.equal(parsed.port, 22028);
});

test("exit codes: ready=0, timeout=3", () => {
  assert.equal(EXIT_OK, 0);
  assert.equal(EXIT_TIMEOUT, 3);
});
