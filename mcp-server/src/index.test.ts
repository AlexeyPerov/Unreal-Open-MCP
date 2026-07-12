import test from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

import {
  createServer,
  handleCallTool,
  handleListTools,
  SERVER_NAME,
  PROJECT_PATH_ENV_VAR,
} from "./index.js";

const here = dirname(fileURLToPath(import.meta.url));
// dist-test layout: dist-test/index.js + dist-test/index.test.js. The compiled
// server entrypoint under test is the sibling dist-test/index.js.
const SERVER_ENTRY = resolve(here, "index.js");

// Empty registry: tools/list returns no tools. This is the P1.5 contract —
// the scaffold must boot and answer before any real tool is wired in.
test("handleListTools returns the (empty) registry", async () => {
  const result = await handleListTools();
  assert.equal(result.tools.length, 0);
});

// Unknown tool → structured MCP error with isError, listing registered names.
// With an empty registry the suffix explicitly states no tools are registered.
test("handleCallTool returns isError for an unknown tool", async () => {
  const result = await handleCallTool({
    params: { name: "unreal_open_mcp_does_not_exist", arguments: {} },
  } as unknown as Parameters<typeof handleCallTool>[0]);
  assert.equal(result.isError, true);
  assert.ok(Array.isArray(result.content));
  const text = (result.content[0] as { type: string; text: string }).text;
  assert.match(text, /Unknown tool:/);
  assert.match(text, /No tools are registered yet/);
});

// createServer wires the handlers without booting stdio. The MCP initialize
// handshake name is the published server identity.
test("createServer returns a Server with the published name", async () => {
  const server = createServer();
  // The Server exposes its name/version via the private `_serverInfo`; we
  // assert the wiring indirectly by confirming it constructed and has the
  // request handlers registered. The handshake is exercised end-to-end in the
  // subprocess test below.
  assert.ok(server);
  assert.equal(typeof server.setRequestHandler, "function");
  await server.close();
});

// --- subprocess / lifecycle tests -----------------------------------------

/**
 * Spawn the compiled entrypoint and drive a minimal MCP initialize →
 * tools/list → EOF handshake over stdio. Confirms:
 *  - the process boots with UNREAL_PROJECT_PATH set,
 *  - initialize reports the published server name,
 *  - tools/list returns an empty array,
 *  - the process exits 0 after stdin EOF (clean disconnect).
 */
test("subprocess: boots, answers initialize + tools/list, exits 0 on EOF", async () => {
  const child = spawn(process.execPath, [SERVER_ENTRY], {
    env: { ...process.env, [PROJECT_PATH_ENV_VAR]: "/tmp/FakeUnrealProject" },
    stdio: ["pipe", "pipe", "pipe"],
  });

  let stdout = "";
  let stderr = "";
  child.stdout.on("data", (chunk: Buffer) => (stdout += chunk.toString()));
  child.stderr.on("data", (chunk: Buffer) => (stderr += chunk.toString()));

  const send = (obj: unknown) =>
    child.stdin.write(`${JSON.stringify(obj)}\n`);

  // MCP initialize handshake.
  send({
    jsonrpc: "2.0",
    id: 1,
    method: "initialize",
    params: {
      protocolVersion: "2024-11-05",
      capabilities: {},
      clientInfo: { name: "test-client", version: "0.0.0" },
    },
  });
  // notifications/initialized — sent after initialize per the spec.
  send({ jsonrpc: "2.0", method: "notifications/initialized" });
  send({ jsonrpc: "2.0", id: 2, method: "tools/list" });

  // Give the server a beat to answer, then EOF stdin to disconnect.
  await new Promise((r) => setTimeout(r, 300));
  child.stdin.end();

  const code = await new Promise<number>((res, rej) => {
    child.on("error", rej);
    child.on("exit", (c) => res(c ?? -1));
  });

  assert.equal(code, 0, `unexpected exit code. stderr:\n${stderr}`);

  const messages = stdout
    .split("\n")
    .filter((line) => line.trim().length > 0)
    .map((line) => JSON.parse(line) as Record<string, unknown>);

  const init = messages.find((m) => m.id === 1) as
    | { result?: { serverInfo?: { name?: string } } }
    | undefined;
  assert.ok(init, "initialize response missing");
  assert.equal(init?.result?.serverInfo?.name, SERVER_NAME);

  const list = messages.find((m) => m.id === 2) as
    | { result?: { tools?: unknown[] } }
    | undefined;
  assert.ok(list, "tools/list response missing");
  assert.deepEqual(list?.result?.tools, []);
});

// Missing UNREAL_PROJECT_PATH → exit 1 with a clear stderr message.
test("subprocess: exits 1 when UNREAL_PROJECT_PATH is missing", async () => {
  const env = { ...process.env };
  delete env[PROJECT_PATH_ENV_VAR];
  const child = spawn(process.execPath, [SERVER_ENTRY], {
    env,
    stdio: ["pipe", "pipe", "pipe"],
  });

  let stderr = "";
  child.stderr.on("data", (chunk: Buffer) => (stderr += chunk.toString()));

  const code = await new Promise<number>((res, rej) => {
    child.on("error", rej);
    child.on("exit", (c) => res(c ?? -1));
  });

  assert.equal(code, 1, `unexpected exit code. stderr:\n${stderr}`);
  assert.match(stderr, new RegExp(PROJECT_PATH_ENV_VAR));
});
