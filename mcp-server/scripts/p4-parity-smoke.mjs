#!/usr/bin/env node
// p4-parity-smoke.mjs — Phase 4 exit-gate parity smoke.
//
// Canonical route: stdio MCP  →  unreal_open_mcp_asset_find  →  bridge
// POST /tools/unreal_open_mcp_asset_find  →  {ok,result,error} envelope.
//
// This is the MANDATORY gate before Phase 5 begins (roadmap Phase 4 exit gate).
// P2.8 proved the first typed-tool round-trip (actor_find) end-to-end; Phase 4
// shipped the asset family (P4.1 asset_find/get, P4.2 CRUD, P4.3 materials,
// P4.4 import). This smoke proves one asset-family tool survives the built
// dist/index.js artifact over stdio — packaging, transport, and
// instance-discovery wiring the in-process integration suite
// (integration.test.ts P4.5 cases) cannot see.
//
// asset_find is the smoke default: read-only (gate-free), so it proves the
// MCP ↔ bridge path without a checkpoint/mutate dance. (material_create is the
// documented alternate if a mutator round-trip is preferred later.)
//
// Three cases are pinned — the three the P4.5 acceptance criteria call out as
// load-bearing:
//   1. HEALTHY — stub serves GET /ping + POST /tools/unreal_open_mcp_asset_find
//      with {ok:true,result:<body>}; assert tools/list advertises asset_find,
//      tools/call returns isError:false, and the INNER result body (not the
//      envelope) survives the round-trip verbatim.
//   2. BRIDGE DOWN — no stub, port pinned to a dead port; assert tools/call
//      surfaces bridge_offline (the asset path inherits P1's failure
//      classification).
//   3. TOOL ERROR — stub returns {ok:false,error:{code,message}}; assert
//      tools/call surfaces isError:true with the tool-specific error code so an
//      agent can branch on invalid_class_path vs a transport error.
//
// Exit code: 0 on green, 1 on any failure. Each step prints ✓/✗ with a short
// detail line; the first failure's raw output is dumped to stderr. Stubs bind
// ephemeral ports (listen(0)) so parallel CI runs never collide.
//
// Live-editor path: to run the same smoke against a real Unreal Editor, pass
// --port <editor bridge port> --project <project path>; the server discovers
// the live bridge the same way. See docs/architecture.md (E2E smoke verification)
// for the failure-signature table.
//
// Adapted from this repo's scripts/p2-parity-smoke.mjs (adapt fidelity, P4.5).
// Intentional delta: the tool under test is the read-only asset_find
// (AssetRegistry) rather than actor_find, and the canonical body is the
// {total,offset,count,assets} pagination envelope rather than actors[].

import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { existsSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const SERVER_ENTRY = resolve(here, "..", "dist", "index.js");

// Canonical 200 /ping body the Unreal bridge emits
// (FUnrealOpenMcpBridgeJson::BuildPingJson). Served by the stub so a healthy
// case stays healthy even if the server preflights /ping.
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

// Canonical asset-find result body the bridge emits — one resolved material
// asset under /Game. The stub wraps this in {ok:true,result:<body>}; the MCP
// text block carries the INNER object after LiveClient.postTool unwraps it.
// This is the parity pin on the pagination envelope + AssetSummary field set
// ({ name, path, package, class }).
const ASSET_FIND_HIT = {
  total: 1,
  offset: 0,
  count: 1,
  assets: [
    {
      name: "M_Test",
      path: "/Game/M_Test.M_Test",
      package: "/Game/M_Test",
      class: "/Script/Engine.Material",
    },
  ],
};

// Tool-error envelope the bridge emits when the handler ran but returned a
// structured failure (e.g. a short dotless class_path rejected before the
// registry query). The {ok:false,error:{code,message}} body surfaces as an MCP
// error carrying the tool-specific code so an agent can branch on the cause.
const ASSET_FIND_ERROR = {
  ok: false,
  error: {
    code: "invalid_class_path",
    message: "class_path 'Material' is not a '/Script/Module.Class' path.",
  },
};

// ---------------------------------------------------------------------------
// arg parsing
// ---------------------------------------------------------------------------

function parseArgs(argv) {
  const opts = {
    project: "/tmp/UnrealOpenMcpSmoke",
    port: null, // null = spawn ephemeral stubs; a number = live bridge
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
  console.error(`Usage: node scripts/p4-parity-smoke.mjs [options]

Options:
  --project <path>   Project path the MCP server binds to (default: /tmp/UnrealOpenMcpSmoke)
  --port <n>         Aim at a live bridge on this port instead of spawning the stub
  -h, --help         Show this help

Default mode spawns ephemeral loopback HTTP stubs and pins the server to them
via UNREAL_OPEN_MCP_BRIDGE_PORT. With --port, the stubs are skipped and the
server discovers / is pinned to the given live bridge port — use this for the
optional manual live-editor smoke (only the healthy case is meaningful there).`);
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
    steps.push(`  ✓ ${label}`);
  } else {
    failed++;
    steps.push(`  ✗ ${label}${detail ? `  -- ${detail}` : ""}`);
  }
  return condition;
}

// ---------------------------------------------------------------------------
// HTTP stub bridge (dispatches by method + URL)
// ---------------------------------------------------------------------------

/**
 * Read a request body to a string. POST /tools/{name} always carries a JSON
 * args body — consuming it keeps the HTTP exchange clean (the client may hold
 * the socket if the body is never drained).
 */
function readBody(req) {
  return new Promise((resolve) => {
    let data = "";
    req.on("data", (chunk) => (data += chunk.toString()));
    req.on("end", () => resolve(data));
  });
}

/**
 * Start an ephemeral loopback HTTP "bridge" whose handler decides what to
 * return by method + URL. Used by the healthy + tool-error cases. Returns
 * {server, port, close}.
 */
function startBridgeStub(port, handler) {
  return new Promise((resolve, reject) => {
    const server = createServer((req, res) => handler(req, res));
    server.on("error", reject);
    server.listen(port, "127.0.0.1", () => {
      const addr = server.address();
      const bound = typeof addr === "object" && addr ? addr.port : port;
      resolve({
        server,
        port: bound,
        close: () => new Promise((r) => server.close(() => r())),
      });
    });
  });
}

/** Handler that serves GET /ping (healthy) + POST asset_find → ok:true. */
async function healthyHandler(req, res) {
  if (req.method === "GET" && req.url === "/ping") {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(HEALTHY_PING));
    return;
  }
  if (
    req.method === "POST" &&
    req.url === "/tools/unreal_open_mcp_asset_find"
  ) {
    await readBody(req);
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ ok: true, result: ASSET_FIND_HIT }));
    return;
  }
  res.writeHead(404, { "Content-Type": "application/json" });
  res.end(
    JSON.stringify({
      error: { code: "not_found", message: `${req.method} ${req.url}` },
    }),
  );
}

/** Handler that serves POST asset_find → ok:false error envelope. */
async function toolErrorHandler(req, res) {
  if (
    req.method === "POST" &&
    req.url === "/tools/unreal_open_mcp_asset_find"
  ) {
    await readBody(req);
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(ASSET_FIND_ERROR));
    return;
  }
  if (req.method === "GET" && req.url === "/ping") {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(HEALTHY_PING));
    return;
  }
  res.writeHead(404, { "Content-Type": "application/json" });
  res.end(
    JSON.stringify({
      error: { code: "not_found", message: `${req.method} ${req.url}` },
    }),
  );
}

// ---------------------------------------------------------------------------
// stdio MCP driver
// ---------------------------------------------------------------------------

/**
 * Spawn the built server and drive initialize → tools/list → tools/call(asset_find).
 * Resolves with the collected stdout JSON-RPC messages + the exit code. The
 * bridgePort pins the server at the stub (or a dead port for the bridge-down
 * case).
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
        clientInfo: { name: "p4-smoke", version: "0.0.0" },
      },
    });
    send({ jsonrpc: "2.0", method: "notifications/initialized" });
    send({ jsonrpc: "2.0", id: 2, method: "tools/list" });
    send({
      jsonrpc: "2.0",
      id: 3,
      method: "tools/call",
      params: {
        name: "unreal_open_mcp_asset_find",
        arguments: { path: "/Game", limit: 10 },
      },
    });

    // Give the server a beat to answer the round-trip, then EOF stdin.
    setTimeout(() => child.stdin.end(), 600);

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

/** Pull the JSON body out of a tools/call result's first text content block. */
function bodyOf(callMsg) {
  const block = callMsg?.result?.content?.[0];
  if (block?.type !== "text" || typeof block.text !== "string") return undefined;
  try {
    return JSON.parse(block.text);
  } catch {
    return undefined;
  }
}

// ---------------------------------------------------------------------------
// cases
// ---------------------------------------------------------------------------

async function runServer({ projectPath, bridgePort }) {
  let outcome;
  try {
    outcome = await driveServer({ projectPath, bridgePort });
  } catch (err) {
    return { error: `Failed to drive server: ${err.message}` };
  }
  return outcome;
}

function assertCommonInit(outcome) {
  // Shared checks across all three cases: the process must exit cleanly and
  // the MCP handshake + tools/list must work regardless of bridge state (they
  // don't touch the bridge).
  check(
    "process exits 0 on stdin EOF",
    outcome.code === 0,
    `got exit ${outcome.code}. stderr tail:\n${(outcome.stderr || "").slice(-400)}`,
  );

  let messages = [];
  try {
    messages = parseMessages(outcome.stdout);
  } catch (err) {
    check("stdout is newline-delimited JSON-RPC", false, err.message);
    return { messages: [], ok: false };
  }
  check("stdout is newline-delimited JSON-RPC", messages.length > 0);

  const init = findById(messages, 1);
  const serverName = init?.result?.serverInfo?.name;
  check(
    "initialize reports server name 'unreal-open-mcp'",
    serverName === "unreal-open-mcp",
    `got ${JSON.stringify(serverName)}`,
  );

  const list = findById(messages, 2);
  const tools = (list?.result?.tools ?? []).map((t) => t.name);
  // The registry grows each phase; assert the two load-bearing tools for THIS
  // smoke (ping for transport parity, asset_find as the typed-tool under
  // test). The full-set pin lives in integration.test.ts (deepEqual) so it
  // breaks loudly there on drift; here we only need the smoke's tool present.
  check(
    "tools/list advertises unreal_open_mcp_ping",
    tools.includes("unreal_open_mcp_ping"),
    `got [${tools.join(", ")}]`,
  );
  check(
    "tools/list advertises unreal_open_mcp_asset_find",
    tools.includes("unreal_open_mcp_asset_find"),
    `got [${tools.join(", ")}]`,
  );

  return { messages, ok: true };
}

async function caseHealthy(opts) {
  console.log("\nCase 1: healthy asset_find round-trip (stub → ok:true)");
  const stub = await startBridgeStub(0, healthyHandler);
  console.log(`  bridge:  stub on 127.0.0.1:${stub.port}`);
  try {
    const outcome = await runServer({
      projectPath: opts.project,
      bridgePort: stub.port,
    });
    if (outcome.error) {
      check("healthy case drove the server", false, outcome.error);
      return;
    }
    const { messages } = assertCommonInit(outcome);
    if (!messages.length) return;

    const call = findById(messages, 3);
    check(
      "tools/call asset_find returns isError:false",
      call?.result?.isError === false,
      `isError=${JSON.stringify(call?.result?.isError)}`,
    );

    const body = bodyOf(call);
    check(
      "asset_find body is valid JSON",
      body !== undefined,
      "first content block was not a JSON text block",
    );
    if (body !== undefined) {
      // The INNER result object (not the {ok,result} envelope) must survive
      // the round-trip verbatim — the parity pin on the asset-find field set.
      check(
        "asset_find body matches the stub result exactly (envelope unwrapped)",
        JSON.stringify(body) === JSON.stringify(ASSET_FIND_HIT),
        `got ${JSON.stringify(body)}`,
      );
    }
  } finally {
    await stub.close();
  }
}

async function caseBridgeDown(opts) {
  console.log("\nCase 2: bridge-down asset_find surfaces bridge_offline (dead port)");
  // Pin to port 1 — nothing listening, ECONNREFUSED. The server starts fine
  // (no preflight ping); tools/list is local; only tools/call(asset_find)
  // hits the dead bridge and must classify as bridge_offline with the
  // instance-lock hint.
  const outcome = await runServer({ projectPath: opts.project, bridgePort: 1 });
  if (outcome.error) {
    check("bridge-down case drove the server", false, outcome.error);
    return;
  }
  const { messages } = assertCommonInit(outcome);
  if (!messages.length) return;

  const call = findById(messages, 3);
  check(
    "tools/call asset_find returns isError:true",
    call?.result?.isError === true,
    `isError=${JSON.stringify(call?.result?.isError)}`,
  );

  const body = bodyOf(call);
  check(
    "bridge-down body is valid JSON",
    body !== undefined,
    "first content block was not a JSON text block",
  );
  if (body !== undefined) {
    check(
      "bridge-down error code is bridge_offline",
      body?.error?.code === "bridge_offline",
      `code=${JSON.stringify(body?.error?.code)}`,
    );
    check(
      "bridge-down message names the instance lock dir",
      /\.unreal-open-mcp\/instances\//.test(body?.error?.message ?? ""),
      `message=${JSON.stringify(body?.error?.message ?? "").slice(0, 160)}`,
    );
  }
}

async function caseToolError(opts) {
  console.log("\nCase 3: tool-error asset_find surfaces the ok:false envelope (stub → ok:false)");
  const stub = await startBridgeStub(0, toolErrorHandler);
  console.log(`  bridge:  stub on 127.0.0.1:${stub.port}`);
  try {
    const outcome = await runServer({
      projectPath: opts.project,
      bridgePort: stub.port,
    });
    if (outcome.error) {
      check("tool-error case drove the server", false, outcome.error);
      return;
    }
    const { messages } = assertCommonInit(outcome);
    if (!messages.length) return;

    const call = findById(messages, 3);
    check(
      "tools/call asset_find returns isError:true",
      call?.result?.isError === true,
      `isError=${JSON.stringify(call?.result?.isError)}`,
    );

    const body = bodyOf(call);
    check(
      "tool-error body is valid JSON",
      body !== undefined,
      "first content block was not a JSON text block",
    );
    if (body !== undefined) {
      // The tool-specific error code rides through so an agent can branch on
      // invalid_class_path rather than seeing an opaque transport error.
      check(
        "tool-error code is invalid_class_path (tool-specific, not transport)",
        body?.error?.code === "invalid_class_path",
        `code=${JSON.stringify(body?.error?.code)}`,
      );
      check(
        "tool-error message rides through verbatim",
        body?.error?.message === ASSET_FIND_ERROR.error.message,
        `message=${JSON.stringify(body?.error?.message)}`,
      );
    }
  } finally {
    await stub.close();
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

async function main() {
  const opts = parseArgs(process.argv.slice(2));

  console.log("unreal-open-mcp — Phase 4 parity smoke (asset_find)");
  console.log(`  server:  ${SERVER_ENTRY}`);
  console.log(`  project: ${opts.project}`);

  if (!existsSync(SERVER_ENTRY)) {
    console.error(`\nBuilt server not found at ${SERVER_ENTRY}.`);
    console.error("Run `npm run build` in mcp-server/ first.");
    process.exit(2);
  }

  // The live-editor path (--port) only exercises the healthy case meaningfully;
  // the bridge-down case needs a dead port and the tool-error case needs a
  // controllable stub, neither of which a live editor provides. When --port is
  // set we still run the healthy case against the live bridge (shape check
  // only — the body won't match ASSET_FIND_HIT) and skip the other two.
  if (opts.port !== null) {
    console.log(`\n--port ${opts.port}: running healthy case against live bridge only.`);
    console.log("  (bridge-down + tool-error cases require the stub harness.)");
    await caseHealthyLive(opts);
  } else {
    await caseHealthy(opts);
    await caseBridgeDown(opts);
    await caseToolError(opts);
  }

  console.log("");
  for (const line of steps) console.log(line);
  console.log("");
  console.log(`${passed} passed, ${failed} failed`);

  if (failed > 0) process.exit(1);
}

/**
 * Live-editor variant of the healthy case (--port). Cannot assert the exact
 * body — only that the round-trip returns isError:false with a recognizable
 * asset-find shape (assets array + numeric total).
 */
async function caseHealthyLive(opts) {
  console.log("\nCase 1 (live): healthy asset_find round-trip (live bridge)");
  console.log(`  bridge:  live on 127.0.0.1:${opts.port}`);
  const outcome = await runServer({
    projectPath: opts.project,
    bridgePort: opts.port,
  });
  if (outcome.error) {
    check("live healthy case drove the server", false, outcome.error);
    return;
  }
  const { messages } = assertCommonInit(outcome);
  if (!messages.length) return;

  const call = findById(messages, 3);
  check(
    "tools/call asset_find returns isError:false",
    call?.result?.isError === false,
    `isError=${JSON.stringify(call?.result?.isError)}`,
  );
  const body = bodyOf(call);
  if (body !== undefined) {
    check(
      "asset_find body carries the assets[] + total shape",
      Array.isArray(body?.assets) && typeof body?.total === "number",
      `got ${JSON.stringify(body).slice(0, 160)}`,
    );
  }
}

main().catch((err) => {
  console.error("p4-parity-smoke fatal:", err);
  process.exit(1);
});
