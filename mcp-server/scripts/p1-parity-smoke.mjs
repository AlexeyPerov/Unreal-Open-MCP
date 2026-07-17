#!/usr/bin/env node
// p1-parity-smoke.mjs — Phase 1 exit-gate parity smoke.
//
// Canonical route: stdio MCP  →  unreal_open_mcp_ping  →  bridge GET /ping.
//
// This is the mandatory gate before Phase 2 work begins (roadmap Phase 1 exit
// gate). It exercises the BUILT dist/index.js artifact over stdio — not the
// in-memory factory covered by integration.test.ts — so it catches packaging,
// transport, and instance-discovery wiring drift that the in-process suite
// cannot see.
//
// Flow:
//   1. Start an ephemeral loopback HTTP "bridge" stub that serves GET /ping
//      with the canonical health payload the Unreal bridge emits.
//   2. Spawn `node dist/index.js` with UNREAL_PROJECT_PATH set and
//      UNREAL_OPEN_MCP_BRIDGE_PORT pinned to the stub port (the env override
//      wins instance-discovery precedence, so the server aims at the stub).
//   3. Drive the MCP handshake over stdio: initialize → tools/list →
//      tools/call(unreal_open_mcp_ping).
//   4. Assert: initialize reports `unreal-open-mcp`; tools/list advertises
//      the registered tool set (ping + the actor tools landed so far); the
//      ping call returns the stub's health body verbatim.
//   5. EOF stdin, assert the process exits 0 (clean disconnect contract).
//
// Exit code: 0 on green, 1 on any failure. Each step prints ✓/✗ with a short
// detail line; the first failure's raw output is dumped to stderr to aid
// debugging. The stub binds an ephemeral port (listen(0)) so parallel CI runs
// never collide.
//
// Live-editor path: to run the same smoke against a real Unreal Editor, drop
// the stub and pass --port <editor bridge port> --project <project path>; the
// server will discover the live bridge the same way. See docs/architecture.md
// (Phase 1 parity smoke) for the failure-signature table.

import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { existsSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const SERVER_ENTRY = resolve(here, "..", "dist", "index.js");

// Canonical 200 /ping body the Unreal bridge emits
// (FUnrealOpenMcpBridgeJson::BuildPingJson). The smoke asserts this exact body
// survives the stdio → MCP → LiveClient → HTTP round-trip — a parity pin on the
// field set the bridge contract pins.
const HEALTHY_PING = {
  connected: true,
  status: "ready",
  projectPath: "/tmp/smoke-project",
  unrealVersion: "5.8.0",
  bridgeVersion: "0.0.1",
  mode: "live",
  port: 21111,
  compiling: false,
  isPlaying: false,
};

// ---------------------------------------------------------------------------
// arg parsing
// ---------------------------------------------------------------------------

function parseArgs(argv) {
  const opts = {
    project: "/tmp/UnrealOpenMcpSmoke",
    port: null, // null = spawn the ephemeral stub; a number = live bridge
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--project") opts.project = argv[++i];
    else if (a === "--port") opts.port = parseInt(argv[++i], 10);
    else if (a === "--help" || a === "-h") {
      printHelp();
      process.exit(0);
    } else {
      console.error(`Unknown argument: ${a}`);
      process.exit(2);
    }
  }
  return opts;
}

function printHelp() {
  console.error(`Usage: node scripts/p1-parity-smoke.mjs [options]

Options:
  --project <path>   Project path the MCP server binds to (default: /tmp/UnrealOpenMcpSmoke)
  --port <n>         Aim at a live bridge on this port instead of spawning the stub
  -h, --help         Show this help

Default mode spawns an ephemeral loopback HTTP stub and pins the server to it
via UNREAL_OPEN_MCP_BRIDGE_PORT. With --port, the stub is skipped and the server
discovers / is pinned to the given live bridge port — use this for the optional
manual live-editor smoke.`);
}

// ---------------------------------------------------------------------------
// tiny test reporter
// ---------------------------------------------------------------------------

const steps = [];
let passed = 0;
let failed = 0;

function check(label, condition, detail = "") {
  if (condition) {
    passed++;
    steps.push(`  \u2713 ${label}`);
  } else {
    failed++;
    steps.push(`  \u2717 ${label}${detail ? `  -- ${detail}` : ""}`);
  }
  return condition;
}

// ---------------------------------------------------------------------------
// HTTP stub bridge
// ---------------------------------------------------------------------------

function startBridgeStub(port, body) {
  return new Promise((resolve, reject) => {
    const server = createServer((req, res) => {
      if (req.url === "/ping") {
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify(body));
      } else {
        res.writeHead(404, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ error: { code: "not_found", message: req.url } }));
      }
    });
    server.on("error", reject);
    server.listen(port, "127.0.0.1", () => {
      const addr = server.address();
      const bound = typeof addr === "object" && addr ? addr.port : port;
      resolve({ server, port: bound });
    });
  });
}

// ---------------------------------------------------------------------------
// stdio MCP driver
// ---------------------------------------------------------------------------

/**
 * Spawn the built server and drive initialize → tools/list → tools/call(ping).
 * Resolves with the collected stdout JSON-RPC messages + the exit code.
 */
function driveServer({ projectPath, bridgePort }) {
  return new Promise((resolveResult, rejectResult) => {
    const env = {
      ...process.env,
      UNREAL_PROJECT_PATH: projectPath,
      UNREAL_OPEN_MCP_BRIDGE_PORT: String(bridgePort),
    };
    // The bearer token (when the live bridge requires one) is discovered from
    // the instance lock by the server itself — there is no token env var. The
    // stub path carries no token; the LiveClient then sends no Authorization
    // header, and the stub answers regardless.

    const child = spawn(process.execPath, [SERVER_ENTRY], {
      env,
      stdio: ["pipe", "pipe", "pipe"],
    });

    let stdout = "";
    let stderr = "";
    child.stdout.on("data", (chunk) => (stdout += chunk.toString()));
    child.stderr.on("data", (chunk) => (stderr += chunk.toString()));

    const send = (obj) => child.stdin.write(`${JSON.stringify(obj)}\n`);

    // MCP initialize handshake.
    send({
      jsonrpc: "2.0",
      id: 1,
      method: "initialize",
      params: {
        protocolVersion: "2024-11-05",
        capabilities: {},
        clientInfo: { name: "p1-smoke", version: "0.0.0" },
      },
    });
    send({ jsonrpc: "2.0", method: "notifications/initialized" });
    send({ jsonrpc: "2.0", id: 2, method: "tools/list" });
    send({
      jsonrpc: "2.0",
      id: 3,
      method: "tools/call",
      params: { name: "unreal_open_mcp_ping", arguments: {} },
    });

    // Give the server a beat to answer the ping round-trip, then EOF stdin.
    setTimeout(() => child.stdin.end(), 500);

    child.on("error", rejectResult);
    child.on("exit", (code) => {
      resolveResult({ stdout, stderr, code: code ?? -1 });
    });
  });
}

/** Parse newline-delimited JSON-RPC messages from stdout. */
function parseMessages(stdout) {
  return stdout
    .split("\n")
    .filter((line) => line.trim().length > 0)
    .map((line) => JSON.parse(line));
}

function findById(messages, id) {
  return messages.find((m) => m.id === id);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

async function main() {
  const opts = parseArgs(process.argv.slice(2));

  console.log("unreal-open-mcp — Phase 1 parity smoke");
  console.log(`  server:  ${SERVER_ENTRY}`);
  console.log(`  project: ${opts.project}`);

  if (!existsSync(SERVER_ENTRY)) {
    console.error(`\nBuilt server not found at ${SERVER_ENTRY}.`);
    console.error("Run `npm run build` in mcp-server/ first.");
    process.exit(2);
  }

  // --- stub bridge (skipped when --port targets a live editor) -------------
  let stub = null;
  let bridgePort = opts.port;
  let expectedBody = HEALTHY_PING;
  if (opts.port === null) {
    stub = await startBridgeStub(0, HEALTHY_PING);
    bridgePort = stub.port;
    console.log(`  bridge:  stub on 127.0.0.1:${bridgePort}`);
  } else {
    console.log(`  bridge:  live on 127.0.0.1:${bridgePort}`);
    // Live path: we cannot assert the exact body, only the shape. Mark it so
    // the ping-body assertion below loosens to a shape check.
    expectedBody = null;
  }
  console.log("");

  let outcome;
  try {
    outcome = await driveServer({
      projectPath: opts.project,
      bridgePort,
    });
  } catch (err) {
    console.error(`Failed to drive server: ${err.message}`);
    if (stub) await closeStub(stub);
    process.exit(1);
  }

  const { stdout, stderr, code } = outcome;

  // --- exit code (clean disconnect contract) -------------------------------
  check(
    "process exits 0 on stdin EOF",
    code === 0,
    `got exit ${code}. stderr tail:\n${stderr.slice(-400)}`,
  );

  let messages = [];
  try {
    messages = parseMessages(stdout);
  } catch (err) {
    check("stdout is newline-delimited JSON-RPC", false, err.message);
  }

  // --- initialize ----------------------------------------------------------
  if (messages.length > 0) {
    const init = findById(messages, 1);
    const serverName = init?.result?.serverInfo?.name;
    check(
      "initialize reports server name 'unreal-open-mcp'",
      serverName === "unreal-open-mcp",
      `got ${JSON.stringify(serverName)}`,
    );

    // --- tools/list -------------------------------------------------------
    const list = findById(messages, 2);
    const tools = (list?.result?.tools ?? []).map((t) => t.name);
    check(
      "tools/list advertises unreal_open_mcp_ping",
      tools.includes("unreal_open_mcp_ping"),
      `got [${tools.join(", ")}]`,
    );
    // The registry grows each phase. Pin the full known set so accidental
    // removal (or drift) is caught here rather than downstream. Kept in sync
    // with the deepEqual pin in integration.test.ts (the canonical registry
    // pin); this smoke re-pins it so a drift the integration suite catches is
    // also caught over the built-artifact path. P2.2 added actor_find; P2.3
    // actor_create; P2.4 actor_modify + object_modify; P2.5 the actor tree +
    // component tools; P2.6 the level lifecycle tools; P2.7 the level inspect
    // + create pair.
    const PINNED_TOOLS = [
      "unreal_open_mcp_ping",
      "unreal_open_mcp_actor_find",
      "unreal_open_mcp_actor_create",
      "unreal_open_mcp_actor_modify",
      "unreal_open_mcp_object_modify",
      "unreal_open_mcp_actor_set_parent",
      "unreal_open_mcp_actor_duplicate",
      "unreal_open_mcp_actor_destroy",
      "unreal_open_mcp_actor_component_add",
      "unreal_open_mcp_actor_component_destroy",
      "unreal_open_mcp_actor_component_get",
      "unreal_open_mcp_actor_component_modify",
      "unreal_open_mcp_actor_component_list_all",
      "unreal_open_mcp_level_open",
      "unreal_open_mcp_level_save",
      "unreal_open_mcp_level_list_loaded",
      "unreal_open_mcp_level_set_current",
      "unreal_open_mcp_level_unload_sublevel",
      "unreal_open_mcp_level_get_data",
      "unreal_open_mcp_level_create",
    ];
    check(
      "tools/list advertises the full registered set",
      tools.length === PINNED_TOOLS.length
        && PINNED_TOOLS.every((t, i) => tools[i] === t),
      `got [${tools.join(", ")}]`,
    );

    // --- tools/call ping --------------------------------------------------
    const call = findById(messages, 3);
    const isError = call?.result?.isError === true;
    check(
      "tools/call ping returns isError:false",
      isError === false,
      `isError=${JSON.stringify(call?.result?.isError)}`,
    );

    const textBlock = call?.result?.content?.[0];
    let body = null;
    if (textBlock?.type === "text" && typeof textBlock.text === "string") {
      try {
        body = JSON.parse(textBlock.text);
      } catch {
        check("ping body is valid JSON", false);
      }
    }

    if (body !== null) {
      // connected:true is the load-bearing field — the game thread answered.
      check(
        "ping body reports connected:true",
        body.connected === true,
        `connected=${JSON.stringify(body.connected)}`,
      );

      if (expectedBody !== null) {
        // Stub path: assert the exact body survived the round-trip verbatim.
        check(
          "ping body matches the stub health payload exactly",
          JSON.stringify(body) === JSON.stringify(expectedBody),
          `got ${JSON.stringify(body)}`,
        );
      } else {
        // Live path: assert the pinned field set is present (shape, not values).
        const requiredFields = [
          "connected",
          "status",
          "projectPath",
          "unrealVersion",
          "bridgeVersion",
          "mode",
          "port",
          "compiling",
          "isPlaying",
        ];
        const present = requiredFields.every((k) => k in body);
        check(
          "ping body carries the pinned health field set",
          present,
          `missing: ${requiredFields.filter((k) => !(k in body)).join(", ")}`,
        );
      }
    }
  }

  if (stub) await closeStub(stub);

  for (const line of steps) console.log(line);
  console.log("");
  console.log(`${passed} passed, ${failed} failed`);

  if (failed > 0) {
    console.error("\n--- stdout (server) ---\n" + stdout.slice(-1200));
    console.error("\n--- stderr (server) ---\n" + stderr.slice(-800));
    process.exit(1);
  }
}

function closeStub(stub) {
  return new Promise((r) => stub.server.close(() => r()));
}

main().catch((err) => {
  console.error("p1-parity-smoke fatal:", err);
  process.exit(1);
});
