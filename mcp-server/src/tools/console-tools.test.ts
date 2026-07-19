import test from "node:test";
import assert from "node:assert/strict";
import { consoleGetLogs } from "./console-get-logs.js";
import { consoleClearLogs } from "./console-clear-logs.js";
import { consoleRunCommand } from "./console-run-command.js";
import { ALL_TOOLS } from "./index.js";

// P5.3 acceptance: the console family is registered under the `unreal_open_mcp_`
// prefix, get-logs advertises the filter set, clear-logs is a no-arg read-only
// tool, and run-command advertises the command arg + mutating gate surface and
// documents the destructive accepted risk.

test("console tools are registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(consoleGetLogs.name, "unreal_open_mcp_console_get_logs");
  assert.equal(consoleClearLogs.name, "unreal_open_mcp_console_clear_logs");
  assert.equal(consoleRunCommand.name, "unreal_open_mcp_console_run_command");
  for (const n of [
    "unreal_open_mcp_console_get_logs",
    "unreal_open_mcp_console_clear_logs",
    "unreal_open_mcp_console_run_command",
  ]) {
    assert.ok(ALL_TOOLS.some((t) => t.name === n), `${n} registered`);
  }
});

test("console_get_logs exposes verbosity/category/contains/limit filters", () => {
  const schema = consoleGetLogs.inputSchema as unknown as {
    properties: Record<string, { enum?: string[]; type?: string }>;
    additionalProperties: boolean;
  };
  assert.ok(schema.properties.verbosity, "verbosity filter");
  assert.ok(
    (schema.properties.verbosity.enum ?? []).includes("warning"),
    "verbosity enum includes warning",
  );
  assert.ok(schema.properties.category, "category filter");
  assert.ok(schema.properties.contains, "contains filter");
  assert.ok(schema.properties.limit, "limit");
  assert.equal(schema.additionalProperties, false);
  const desc = consoleGetLogs.description ?? "";
  assert.match(desc, /read-only/i);
  assert.match(desc, /sequence/);
  assert.match(desc, /truncated/);
  // Documents it is not a full mirror / distinct from call-scoped logs[].
  assert.match(desc, /cold-start|not captured|window/i);
});

test("console_clear_logs is a no-arg read-only tool returning removed", () => {
  const schema = consoleClearLogs.inputSchema as unknown as {
    properties: Record<string, unknown>;
    additionalProperties: boolean;
  };
  assert.equal(Object.keys(schema.properties ?? {}).length, 0);
  assert.equal(schema.additionalProperties, false);
  const desc = consoleClearLogs.description ?? "";
  assert.match(desc, /removed/);
  assert.match(desc, /read-only/i);
});

test("console_run_command requires command and documents the gate + risk", () => {
  const schema = consoleRunCommand.inputSchema as unknown as {
    required?: string[];
    properties: Record<string, unknown>;
  };
  assert.deepEqual(schema.required, ["command"]);
  assert.ok(schema.properties.command, "command arg");
  assert.ok(schema.properties.paths_hint, "paths_hint");
  assert.ok(schema.properties.gate, "gate");
  const desc = consoleRunCommand.description ?? "";
  assert.match(desc, /paths_hint_required/, "documents the gate hint rule");
  assert.match(desc, /destructive|WARNING/i, "documents the destructive risk");
  assert.match(desc, /handled/, "documents the handled result field");
});
