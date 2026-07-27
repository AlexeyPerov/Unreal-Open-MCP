#!/usr/bin/env node
// p6-parity-smoke.mjs — Blueprint-family compile-loop parity smoke.
//
// Canonical route: stdio MCP  →  unreal_open_mcp_blueprint_spawn  →  bridge
// POST /tools/unreal_open_mcp_blueprint_spawn  →  {ok,result,error} envelope.
//
// E2E smoke for the Blueprint compile loop. Proves the two load-bearing
// compile-loop tools (blueprint_spawn + blueprint_compile) survive the built
// dist/index.js artifact over stdio — packaging, transport, and instance-
// discovery wiring the in-process integration suite cannot see.
//
// Four cases are pinned:
//   1. HEALTHY SPAWN — stub serves GET /ping + POST
//      /tools/unreal_open_mcp_blueprint_spawn with {ok:true,result:<body>};
//      assert tools/list advertises blueprint_spawn, tools/call returns
//      isError:false, and the INNER result body (not the envelope) survives
//      the round-trip verbatim.
//   2. BRIDGE DOWN — no stub, port pinned to a dead port; assert tools/call
//      surfaces bridge_offline (the Blueprint path inherits the standard
//      transport-failure classification).
//   3. TOOL ERROR — stub returns {ok:false,error:{code,message}}; assert
//      tools/call surfaces isError:true with the tool-specific error code so
//      an agent can branch on not_compiled vs a transport error.
//   4. COMPILE FAILED = DATA — stub returns
//      {ok:true,result:{succeeded:false, numErrors:1, messages:[...]}}; assert
//      tools/call returns isError:false (a failed compile is a NORMAL result,
//      NOT a transport failure — the diagnostics ride through as data). This
//      pins the "succeeded:false is data" contract at the stdio layer.
//
// Exit code: 0 on green, 1 on any failure. Each step prints ✓/✗ with a short
// detail line; the first failure's raw output is dumped to stderr. Stubs bind
// ephemeral ports (listen(0)) so parallel CI runs never collide.
//
// Live-editor path: to run the same smoke against a real Unreal Editor, pass
// --port <editor bridge port> --project <project path>; the server discovers
// the live bridge the same way. See docs/architecture.md (E2E smoke
// verification) for the failure-signature table.
//
// Adapted from this repo's scripts/p4-parity-smoke.mjs (adapt fidelity).
// Intentional delta: the tool under test is the mutating blueprint_spawn
// (loop-closer) rather than the read-only asset_find, the canonical body is
// the spawn identity { actor, name, class, path, location }, AND this smoke
// adds the compile-failure data-path case (case 4) that pins the
// "succeeded:false is data" contract at the stdio layer.

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

// Canonical blueprint_spawn result body the bridge emits — minimal actor
// identity for the agent to chain from. The stub wraps this in
// {ok:true,result:<body>}; the MCP text block carries the INNER object after
// LiveClient.postTool unwraps it. This is the parity pin on the spawn
// identity field set ({ actor, name, class, path, location }).
const SPAWN_OK = {
  actor: "BP_Smoke",
  name: "BP_Smoke_C_0",
  class: "/Game/McpTemp/BP_Smoke.BP_Smoke_C",
  path: "/Game/Maps/UEDPIE_0_TestMap.TestMap:PersistentLevel.BP_Smoke_C_0",
  location: { x: 0, y: 0, z: 100 },
};

// Tool-error envelope the bridge emits when the handler ran but returned a
// structured failure — here an uncompiled Blueprint (no GeneratedClass). The
// {ok:false,error:{code,message}} body surfaces as an MCP error carrying the
// tool-specific not_compiled code so an agent can branch on the cause (run
// blueprint_compile) instead of retrying blindly.
const SPAWN_NOT_COMPILED = {
  ok: false,
  error: {
    code: "not_compiled",
    message:
      "Blueprint 'BP_New' has no GeneratedClass — run blueprint_compile first.",
  },
};

// Compile-FAILED data envelope — the P6.5 contract: succeeded:false on an
// ok:true envelope. The bridge ran the compile, the compiler reported errors,
// but the dispatch succeeded — so MCP isError MUST stay false and the
// diagnostics ride through as data, NOT as a transport error.
const COMPILE_FAILED = {
  ok: true,
  result: {
    succeeded: false,
    numErrors: 1,
    numWarnings: 0,
    messages: [
      {
        severity: "error",
        message: "Foo node: pin 'A' is not connected.",
        node: "Foo",
        graph: "EventGraph",
      },
    ],
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
  console.error(`Usage: node scripts/p6-parity-smoke.mjs [options]

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

function readBody(req) {
  return new Promise((resolve) => {
    let data = "";
    req.on("data", (chunk) => (data += chunk.toString()));
    req.on("end", () => resolve(data));
  });
}

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

/** Handler that serves GET /ping (healthy) + POST blueprint_spawn → ok:true. */
async function healthySpawnHandler(req, res) {
  if (req.method === "GET" && req.url === "/ping") {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(HEALTHY_PING));
    return;
  }
  if (
    req.method === "POST" &&
    req.url === "/tools/unreal_open_mcp_blueprint_spawn"
  ) {
    await readBody(req);
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ ok: true, result: SPAWN_OK }));
    return;
  }
  res.writeHead(404, { "Content-Type": "application/json" });
  res.end(
    JSON.stringify({
      error: { code: "not_found", message: `${req.method} ${req.url}` },
    }),
  );
}

/** Handler that serves POST blueprint_spawn → ok:false error envelope. */
async function spawnErrorHandler(req, res) {
  if (
    req.method === "POST" &&
    req.url === "/tools/unreal_open_mcp_blueprint_spawn"
  ) {
    await readBody(req);
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(SPAWN_NOT_COMPILED));
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

/** Handler that serves POST blueprint_compile → succeeded:false data envelope
 *  (the P6.5 contract case). */
async function compileFailedHandler(req, res) {
  if (req.method === "GET" && req.url === "/ping") {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(HEALTHY_PING));
    return;
  }
  if (
    req.method === "POST" &&
    req.url === "/tools/unreal_open_mcp_blueprint_compile"
  ) {
    await readBody(req);
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(COMPILE_FAILED));
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
 * Spawn the built server and drive initialize → tools/list → tools/call.
 * The `tool` arg selects which blueprint tool the smoke drives, and `args`
 * carries its arguments. Resolves with the collected stdout JSON-RPC messages
 * + the exit code.
 */
function driveServer({ projectPath, bridgePort, tool, args }) {
  return new Promise((resolveResult, rejectResult) => {
    const env = {
      ...process.env,
      UNREAL_PROJECT_PATH: projectPath,
      UNREAL_OPEN_MCP_BRIDGE_PORT: String(bridgePort),
    };

    const child = spawn(process.execPath, [SERVER_ENTRY], {
      env,
      stdio: ["pipe", "pipe", "pipe"],
    });

    let stdout = "";
    let stderr = "";
    child.stdout.on("data", (chunk) => (stdout += chunk.toString()));
    child.stderr.on("data", (chunk) => (stderr += chunk.toString()));

    const send = (obj) => child.stdin.write(`${JSON.stringify(obj)}\n`);

    send({
      jsonrpc: "2.0",
      id: 1,
      method: "initialize",
      params: {
        protocolVersion: "2024-11-05",
        capabilities: {},
        clientInfo: { name: "p6-smoke", version: "0.0.0" },
      },
    });
    send({ jsonrpc: "2.0", method: "notifications/initialized" });
    send({ jsonrpc: "2.0", id: 2, method: "tools/list" });
    send({
      jsonrpc: "2.0",
      id: 3,
      method: "tools/call",
      params: { name: tool, arguments: args },
    });

    // Give the server a beat to answer the round-trip, then EOF stdin.
    setTimeout(() => child.stdin.end(), 600);

    child.on("error", rejectResult);
    child.on("exit", (code) => {
      resolveResult({ stdout, stderr, code: code ?? -1 });
    });
  });
}

function parseMessages(stdout) {
  return stdout
    .split("\n")
    .filter((line) => line.trim().length > 0)
    .map((line) => JSON.parse(line));
}

function findById(messages, id) {
  return messages.find((m) => m.id === id);
}

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
// shared runner
// ---------------------------------------------------------------------------

async function runServer(opts) {
  let outcome;
  try {
    outcome = await driveServer(opts);
  } catch (err) {
    return { error: `Failed to drive server: ${err.message}` };
  }
  return outcome;
}

function assertCommonInit(outcome) {
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
  // smoke (ping for transport parity, blueprint_spawn as the typed-tool under
  // test). The full-set pin lives in integration.test.ts; here we only need
  // the smoke's tool present.
  check(
    "tools/list advertises unreal_open_mcp_ping",
    tools.includes("unreal_open_mcp_ping"),
    `got [${tools.join(", ")}]`,
  );
  check(
    "tools/list advertises unreal_open_mcp_blueprint_spawn",
    tools.includes("unreal_open_mcp_blueprint_spawn"),
    `got [${tools.join(", ")}]`,
  );

  return { messages, ok: true };
}

// ---------------------------------------------------------------------------
// cases
// ---------------------------------------------------------------------------

async function caseHealthySpawn(opts) {
  console.log("\nCase 1: healthy blueprint_spawn round-trip (stub → ok:true)");
  const stub = await startBridgeStub(0, healthySpawnHandler);
  console.log(`  bridge:  stub on 127.0.0.1:${stub.port}`);
  try {
    const outcome = await runServer({
      projectPath: opts.project,
      bridgePort: stub.port,
      tool: "unreal_open_mcp_blueprint_spawn",
      args: {
        path: "/Game/McpTemp/BP_Smoke",
        location: { x: 0, y: 0, z: 100 },
        paths_hint: ["/Game/McpTemp/BP_Smoke"],
      },
    });
    if (outcome.error) {
      check("healthy case drove the server", false, outcome.error);
      return;
    }
    const { messages } = assertCommonInit(outcome);
    if (!messages.length) return;

    const call = findById(messages, 3);
    check(
      "tools/call blueprint_spawn returns isError:false",
      call?.result?.isError === false,
      `isError=${JSON.stringify(call?.result?.isError)}`,
    );

    const body = bodyOf(call);
    check(
      "blueprint_spawn body is valid JSON",
      body !== undefined,
      "first content block was not a JSON text block",
    );
    if (body !== undefined) {
      // The INNER result object (not the {ok,result} envelope) must survive
      // the round-trip verbatim — the parity pin on the spawn identity field
      // set.
      check(
        "blueprint_spawn body matches the stub result exactly (envelope unwrapped)",
        JSON.stringify(body) === JSON.stringify(SPAWN_OK),
        `got ${JSON.stringify(body)}`,
      );
    }
  } finally {
    await stub.close();
  }
}

async function caseBridgeDown(opts) {
  console.log("\nCase 2: bridge-down blueprint_spawn surfaces bridge_offline (dead port)");
  // Pin to port 1 — nothing listening, ECONNREFUSED. The server starts fine
  // (no preflight ping); tools/list is local; only tools/call(blueprint_spawn)
  // hits the dead bridge and must classify as bridge_offline with the
  // instance-lock hint.
  const outcome = await runServer({
    projectPath: opts.project,
    bridgePort: 1,
    tool: "unreal_open_mcp_blueprint_spawn",
    args: {
      path: "/Game/McpTemp/BP_Smoke",
      paths_hint: ["/Game/McpTemp/BP_Smoke"],
    },
  });
  if (outcome.error) {
    check("bridge-down case drove the server", false, outcome.error);
    return;
  }
  const { messages } = assertCommonInit(outcome);
  if (!messages.length) return;

  const call = findById(messages, 3);
  check(
    "tools/call blueprint_spawn returns isError:true",
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
  console.log("\nCase 3: tool-error blueprint_spawn surfaces the ok:false envelope (stub → ok:false)");
  const stub = await startBridgeStub(0, spawnErrorHandler);
  console.log(`  bridge:  stub on 127.0.0.1:${stub.port}`);
  try {
    const outcome = await runServer({
      projectPath: opts.project,
      bridgePort: stub.port,
      tool: "unreal_open_mcp_blueprint_spawn",
      args: {
        path: "/Game/McpTemp/BP_New",
        paths_hint: ["/Game/McpTemp/BP_New"],
      },
    });
    if (outcome.error) {
      check("tool-error case drove the server", false, outcome.error);
      return;
    }
    const { messages } = assertCommonInit(outcome);
    if (!messages.length) return;

    const call = findById(messages, 3);
    check(
      "tools/call blueprint_spawn returns isError:true",
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
      // not_compiled (run blueprint_compile) rather than seeing an opaque
      // transport error.
      check(
        "tool-error code is not_compiled (tool-specific, not transport)",
        body?.error?.code === "not_compiled",
        `code=${JSON.stringify(body?.error?.code)}`,
      );
      check(
        "tool-error message rides through verbatim",
        body?.error?.message === SPAWN_NOT_COMPILED.error.message,
        `message=${JSON.stringify(body?.error?.message)}`,
      );
    }
  } finally {
    await stub.close();
  }
}

async function caseCompileFailedData(opts) {
  console.log("\nCase 4: compile succeeded:false stays isError:false (compile failure is data, not transport)");
  const stub = await startBridgeStub(0, compileFailedHandler);
  console.log(`  bridge:  stub on 127.0.0.1:${stub.port}`);
  try {
    const outcome = await runServer({
      projectPath: opts.project,
      bridgePort: stub.port,
      tool: "unreal_open_mcp_blueprint_compile",
      args: {
        path: "/Game/McpTemp/BP_Broken",
        paths_hint: ["/Game/McpTemp/BP_Broken"],
      },
    });
    if (outcome.error) {
      check("compile-failed case drove the server", false, outcome.error);
      return;
    }
    const { messages } = assertCommonInit(outcome);
    if (!messages.length) return;

    const call = findById(messages, 3);
    // CRITICAL P6.5 invariant: a failed compile is a NORMAL result, NOT a
    // transport failure. The envelope stays ok:true with succeeded:false + a
    // populated messages[], so MCP isError MUST stay false and the
    // diagnostics ride through as data.
    check(
      "tools/call blueprint_compile with succeeded:false returns isError:false (data, not error)",
      call?.result?.isError === false,
      `isError=${JSON.stringify(call?.result?.isError)}`,
    );

    const body = bodyOf(call);
    check(
      "compile-failed body is valid JSON",
      body !== undefined,
      "first content block was not a JSON text block",
    );
    if (body !== undefined) {
      check(
        "compile-failed body carries succeeded:false",
        body?.succeeded === false,
        `succeeded=${JSON.stringify(body?.succeeded)}`,
      );
      check(
        "compile-failed body carries the error count",
        body?.numErrors === 1,
        `numErrors=${JSON.stringify(body?.numErrors)}`,
      );
      check(
        "compile-failed body carries the diagnostics messages[]",
        Array.isArray(body?.messages) && body.messages.length === 1,
        `messages=${JSON.stringify(body?.messages).slice(0, 160)}`,
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

  console.log("unreal-open-mcp — Blueprint compile-loop parity smoke");
  console.log(`  server:  ${SERVER_ENTRY}`);
  console.log(`  project: ${opts.project}`);

  if (!existsSync(SERVER_ENTRY)) {
    console.error(`\nBuilt server not found at ${SERVER_ENTRY}.`);
    console.error("Run `npm run build` in mcp-server/ first.");
    process.exit(2);
  }

  // The live-editor path (--port) only exercises the healthy spawn case
  // meaningfully; the other three cases need a controllable stub. When --port
  // is set we still run the healthy spawn against the live bridge (shape
  // check only — the body won't match SPAWN_OK) and skip the rest.
  if (opts.port !== null) {
    console.log(`\n--port ${opts.port}: running healthy spawn case against live bridge only.`);
    console.log("  (bridge-down + tool-error + compile-failed cases require the stub harness.)");
    await caseHealthySpawnLive(opts);
  } else {
    await caseHealthySpawn(opts);
    await caseBridgeDown(opts);
    await caseToolError(opts);
    await caseCompileFailedData(opts);
  }

  console.log("");
  for (const line of steps) console.log(line);
  console.log("");
  console.log(`${passed} passed, ${failed} failed`);

  if (failed > 0) process.exit(1);
}

/**
 * Live-editor variant of the healthy spawn case (--port). Cannot assert the
 * exact body — only that the round-trip returns isError:false with a
 * recognizable spawn shape (actor + class + location).
 */
async function caseHealthySpawnLive(opts) {
  console.log("\nCase 1 (live): healthy blueprint_spawn round-trip (live bridge)");
  console.log(`  bridge:  live on 127.0.0.1:${opts.port}`);
  const outcome = await runServer({
    projectPath: opts.project,
    bridgePort: opts.port,
    tool: "unreal_open_mcp_blueprint_spawn",
    args: {
      path: "/Game/McpTemp/BP_Smoke",
      paths_hint: ["/Game/McpTemp/BP_Smoke"],
    },
  });
  if (outcome.error) {
    check("live healthy spawn case drove the server", false, outcome.error);
    return;
  }
  const { messages } = assertCommonInit(outcome);
  if (!messages.length) return;

  const call = findById(messages, 3);
  check(
    "tools/call blueprint_spawn returns isError:false",
    call?.result?.isError === false,
    `isError=${JSON.stringify(call?.result?.isError)}`,
  );
  const body = bodyOf(call);
  if (body !== undefined) {
    check(
      "blueprint_spawn body carries the actor + class + location shape",
      typeof body?.actor === "string" &&
        typeof body?.class === "string" &&
        typeof body?.location === "object",
      `got ${JSON.stringify(body).slice(0, 160)}`,
    );
  }
}

main().catch((err) => {
  console.error("p6-parity-smoke fatal:", err);
  process.exit(1);
});
