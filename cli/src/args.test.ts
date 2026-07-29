// Tests for the CLI argument parser (src/args.ts) and the runCli dispatcher's
// fast paths (src/cli.ts). Pure-function + captured-stdout tests — no I/O, no
// process teardown.
//
// Built + run via the package test config (see package.json `test`):
//   tsc -p tsconfig.test.json  &&  node --test 'dist-test/**/*.test.js'

import { test } from "node:test";
import assert from "node:assert/strict";

import {
  parseCliArgs,
  parsePort,
  KNOWN_COMMANDS,
  IMPLEMENTED_COMMANDS,
  type ParsedCli,
} from "./args.js";
import { runCli } from "./cli.js";
import { helpText, versionText } from "./help-text.js";
import { BIN_NAME } from "./constants.js";

function parse(argv: string[]): ParsedCli {
  return parseCliArgs(argv);
}

// ---------------------------------------------------------------------------
// command recognition
// ---------------------------------------------------------------------------

test("parseCliArgs: recognizes every known command", () => {
  for (const cmd of KNOWN_COMMANDS) {
    const p = parse([cmd]);
    assert.equal(p.command, cmd);
    assert.equal(p.error, undefined);
  }
});

test("parseCliArgs: --help short-circuits to help command", () => {
  assert.equal(parse(["--help"]).command, "help");
  assert.equal(parse(["-h"]).command, "help");
});

test("parseCliArgs: --version short-circuits to version command", () => {
  assert.equal(parse(["--version"]).command, "version");
  assert.equal(parse(["-V"]).command, "version");
});

test("parseCliArgs: unknown command is an error", () => {
  const p = parse(["bogus"]);
  assert.equal(p.command, null);
  assert.match(p.error ?? "", /Unknown command 'bogus'/);
});

test("parseCliArgs: no argv → no command", () => {
  const p = parse([]);
  assert.equal(p.command, null);
  assert.equal(p.error, undefined);
});

// ---------------------------------------------------------------------------
// shared global flags
// ---------------------------------------------------------------------------

test("parseCliArgs: --json is captured", () => {
  assert.equal(parse(["status", "--json"]).json, true);
});

test("parseCliArgs: -v / --verbose are captured", () => {
  assert.equal(parse(["status", "-v"]).verbose, true);
  assert.equal(parse(["status", "--verbose"]).verbose, true);
});

test("parseCliArgs: --project / -P override", () => {
  assert.equal(parse(["status", "--project", "/p"]).projectPath, "/p");
  assert.equal(parse(["status", "-P", "/p"]).projectPath, "/p");
});

test("parseCliArgs: --project requires a value", () => {
  assert.match(parse(["status", "--project"]).error ?? "", /--project/);
  assert.match(parse(["status", "--project", "--json"]).error ?? "", /--project/);
});

test("parseCliArgs: --port / -p override parses to a number", () => {
  assert.equal(parse(["status", "--port", "23456"]).port, 23456);
  assert.equal(parse(["status", "-p", "23456"]).port, 23456);
});

test("parseCliArgs: --port rejects non-numeric / out-of-range", () => {
  assert.match(parse(["status", "--port", "abc"]).error ?? "", /--port/);
  assert.match(parse(["status", "--port", "-5"]).error ?? "", /--port/);
  assert.match(parse(["status", "--port", "0"]).error ?? "", /--port/);
  assert.match(parse(["status", "--port", "70000"]).error ?? "", /--port/);
});

test("parseCliArgs: --port requires a value", () => {
  assert.match(parse(["status", "--port"]).error ?? "", /--port/);
});

test("parseCliArgs: unknown flag is an error", () => {
  const p = parse(["status", "--nonsense"]);
  assert.deepEqual(p.unknown, ["--nonsense"]);
  assert.match(p.error ?? "", /Unknown option/);
});

// ---------------------------------------------------------------------------
// positionals
// ---------------------------------------------------------------------------

test("parseCliArgs: extra positionals after the command are collected", () => {
  const p = parse(["install-plugin", "--project", "/p", "extra1", "extra2"]);
  assert.equal(p.command, "install-plugin");
  assert.deepEqual(p.positionals, ["extra1", "extra2"]);
});

test("parseCliArgs: flags may appear before or after the command", () => {
  assert.equal(parse(["--json", "status"]).command, "status");
  assert.equal(parse(["--json", "status"]).json, true);
  assert.equal(parse(["status", "--json"]).json, true);
});

// ---------------------------------------------------------------------------
// parsePort
// ---------------------------------------------------------------------------

test("parsePort: accepts in-range integers", () => {
  assert.equal(parsePort("1"), 1);
  assert.equal(parsePort("8080"), 8080);
  assert.equal(parsePort("65535"), 65535);
});

test("parsePort: rejects out-of-range / non-integer / undefined", () => {
  assert.equal(parsePort("0"), undefined);
  assert.equal(parsePort("65536"), undefined);
  assert.equal(parsePort("1.5"), undefined);
  assert.equal(parsePort("abc"), undefined);
  assert.equal(parsePort(undefined), undefined);
  assert.equal(parsePort("-1"), undefined);
});

// ---------------------------------------------------------------------------
// runCli fast paths
// ---------------------------------------------------------------------------

/**
 * Capture everything runCli writes to a stream (stdout or stderr) while `fn`
 * runs. Returns the concatenated string. Restores the original write in finally.
 *
 * runCli's writes go through `writeAndDrain`, which uses the
 * `write(chunk, callback)` overload and awaits the callback. The fake must
 * therefore accept AND invoke that callback (synchronously) or writeAndDrain's
 * promise never resolves and the test hangs.
 */
async function captureStream(
  target: "stdout" | "stderr",
  fn: () => Promise<unknown>,
): Promise<string> {
  const stream = process[target];
  const chunks: string[] = [];
  const real = stream.write.bind(stream);
  (stream.write as unknown) = (chunk: unknown, cb?: (err?: Error | null) => void) => {
    chunks.push(typeof chunk === "string" ? chunk : String(chunk));
    if (typeof cb === "function") cb(null);
    return true;
  };
  try {
    await fn();
  } finally {
    (stream.write as unknown) = real;
  }
  return chunks.join("");
}

test("runCli(--version) prints 'unreal-open-mcp-cli <version>' and exits 0", async () => {
  const out = await captureStream("stdout", () =>
    runCli({ version: "9.9.9", argv: ["--version"] }),
  );
  assert.equal(out, `${BIN_NAME} 9.9.9\n`);
  const outcome = await runCli({ version: "9.9.9", argv: ["--version"] });
  assert.equal(outcome.handled, true);
  assert.equal(outcome.exitCode, 0);
});

test("runCli(-V) short form matches --version", async () => {
  const out = await captureStream("stdout", () =>
    runCli({ version: "1.2.3", argv: ["-V"] }),
  );
  assert.equal(out, `${BIN_NAME} 1.2.3\n`);
});

test("runCli(--help) prints the help text and exits 0", async () => {
  const expected = helpText(BIN_NAME) + "\n";
  const out = await captureStream("stdout", () =>
    runCli({ version: "0.0.0", argv: ["--help"] }),
  );
  assert.equal(out, expected);
});

test("runCli(-h) short form matches --help", async () => {
  const expected = helpText(BIN_NAME) + "\n";
  const out = await captureStream("stdout", () =>
    runCli({ version: "0.0.0", argv: ["-h"] }),
  );
  assert.equal(out, expected);
});

test("runCli(--version) does not consult UNREAL_PROJECT_PATH", async () => {
  const saved = process.env.UNREAL_PROJECT_PATH;
  delete process.env.UNREAL_PROJECT_PATH;
  try {
    const out = await captureStream("stdout", () =>
      runCli({ version: "0.7.0", argv: ["--version"] }),
    );
    assert.equal(out, `${BIN_NAME} 0.7.0\n`);
  } finally {
    if (saved !== undefined) process.env.UNREAL_PROJECT_PATH = saved;
  }
});

test("runCli(no command) prints help and exits 0 (no server fallthrough)", async () => {
  const expected = helpText(BIN_NAME) + "\n";
  const out = await captureStream("stdout", () =>
    runCli({ version: "0.0.0", argv: [] }),
  );
  assert.equal(out, expected);
  const outcome = await runCli({ version: "0.0.0", argv: [] });
  assert.equal(outcome.handled, true);
  assert.equal(outcome.exitCode, 0);
});

test("runCli(unknown command) writes the error + help to stderr and exits 2", async () => {
  const out = await captureStream("stderr", () =>
    runCli({ version: "0.0.0", argv: ["bogus"] }),
  );
  assert.match(out, /Unknown command 'bogus'/);
  const outcome = await runCli({ version: "0.0.0", argv: ["bogus"] });
  assert.equal(outcome.handled, true);
  assert.equal(outcome.exitCode, 2);
});

test("runCli(unknown option) writes the error to stderr and exits 2", async () => {
  const outcome = await runCli({ version: "0.0.0", argv: ["status", "--nope"] });
  assert.equal(outcome.handled, true);
  assert.equal(outcome.exitCode, 2);
});

test("every recognized command is implemented (no not-implemented branch reachable)", () => {
  // P8.5: all six commands now have real handlers. The not-implemented branch
  // in cli.ts is retained as a guard for future commands, but no recognized
  // command reaches it today. This guards against a stub shipping before its
  // module lands in either direction.
  const unimplemented = KNOWN_COMMANDS.filter((c) => !IMPLEMENTED_COMMANDS.includes(c));
  assert.deepEqual(unimplemented, [], `expected every recognized command implemented, but found: ${unimplemented.join(", ")}`);
});

// ---------------------------------------------------------------------------
// help-text module contract
// ---------------------------------------------------------------------------

test("helpText mentions every known command and the version line", () => {
  const text = helpText(BIN_NAME);
  for (const cmd of KNOWN_COMMANDS) {
    assert.ok(text.includes(cmd), `helpText mentions ${cmd}`);
  }
  assert.match(text, /--version/);
});

test("versionText formats as '<bin> <version>'", () => {
  assert.equal(versionText("5.5.5"), `${BIN_NAME} 5.5.5`);
  assert.equal(versionText("5.5.5", "custom-bin"), "custom-bin 5.5.5");
});

test("IMPLEMENTED_COMMANDS lists every recognized command", () => {
  // P8.5: the CLI track is complete — all six recognized commands have real
  // handlers. Guards against an accidental stub shipping before its module
  // lands (or a recognized command missing its dispatcher branch).
  assert.deepEqual([...IMPLEMENTED_COMMANDS], [
    "install-plugin",
    "setup-mcp",
    "open",
    "wait-for-ready",
    "status",
    "configure",
  ]);
});

// ---------------------------------------------------------------------------
// install-plugin options (parsed globally, consumed by the command)
// ---------------------------------------------------------------------------

test("parseCliArgs: --plugin-source requires a value", () => {
  assert.match(parse(["install-plugin", "--plugin-source"]).error ?? "", /--plugin-source/);
});

test("parseCliArgs: --plugin-source captures the value", () => {
  assert.equal(
    parse(["install-plugin", "--plugin-source", "/mono/root"]).pluginSource,
    "/mono/root",
  );
});

test("parseCliArgs: --symlink / --with-verify / --no-verify / --dry-run flags", () => {
  assert.equal(parse(["install-plugin", "--symlink"]).symlink, true);
  assert.equal(parse(["install-plugin", "--with-verify"]).withVerify, true);
  assert.equal(parse(["install-plugin", "--no-verify"]).withVerify, false);
  assert.equal(parse(["install-plugin", "--dry-run"]).dryRun, true);
  // defaults
  const p = parse(["install-plugin"]);
  assert.equal(p.symlink, false);
  assert.equal(p.withVerify, undefined);
  assert.equal(p.dryRun, false);
});

// ---------------------------------------------------------------------------
// setup-mcp options (parsed globally, consumed by the command)
// ---------------------------------------------------------------------------

test("parseCliArgs: --list flag is captured", () => {
  assert.equal(parse(["setup-mcp", "--list"]).list, true);
  // defaults
  assert.equal(parse(["setup-mcp"]).list, false);
});

test("parseCliArgs: --server-command captures the value", () => {
  assert.equal(
    parse(["setup-mcp", "cursor", "--server-command", "node"]).serverCommand,
    "node",
  );
});

test("parseCliArgs: --server-command requires a value", () => {
  assert.match(parse(["setup-mcp", "--server-command"]).error ?? "", /--server-command/);
  assert.match(parse(["setup-mcp", "--server-command", "--json"]).error ?? "", /--server-command/);
});

test("parseCliArgs: setup-mcp agent lands as a positional", () => {
  const p = parse(["setup-mcp", "cursor"]);
  assert.equal(p.command, "setup-mcp");
  assert.deepEqual(p.positionals, ["cursor"]);
});
