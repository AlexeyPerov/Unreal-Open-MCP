#!/usr/bin/env node
// p7-parity-smoke.mjs — Source-family compile-loop parity smoke.
//
// Canonical route: stdio MCP  →  unreal_open_mcp_source_update  →  bridge
// POST /tools/unreal_open_mcp_source_update  →  {ok,result,error} envelope,
// then unreal_open_mcp_source_compile  →  structured C++ diagnostic report.
//
// E2E smoke for the C++ source compile loop. Proves the two load-bearing
// source tools (source_update + source_compile) survive the built
// dist/index.js artifact over stdio — packaging, transport, and instance-
// discovery wiring the in-process integration suite cannot see. This is the
// mandatory phase-gate smoke for Phase 7 (source / C++ compile).
//
// Four cases are pinned:
//   1. HEALTHY UPDATE + COMPILE — stub serves GET /ping + POST
//      /tools/unreal_open_mcp_source_update AND
//      /tools/unreal_open_mcp_source_compile with {ok:true,result:<body>};
//      assert tools/list advertises both tools, tools/call(source_update)
//      returns isError:false with the INNER {path,mode,bytes_written,
//      lines_written} body surviving the round-trip verbatim, and
//      tools/call(source_compile) returns isError:false with a clean UBT
//      report ({method:'ubt', success:true, compile_clean:true,
//      error_count:0, diagnostics:[]}).
//   2. BRIDGE DOWN — no stub, port pinned to a dead port; assert tools/call
//      surfaces bridge_offline (the Source path inherits the standard
//      transport-failure classification).
//   3. TOOL ERROR — stub returns {ok:false,error:{code,message}} for
//      source_update (path_escapes_jail — an agent passing ../ or an absolute
//      path outside Source/); assert tools/call surfaces isError:true with
//      the tool-specific error code so an agent can branch on the jail
//      rejection vs a transport error.
//   4. COMPILE FAILED = DATA — stub returns {ok:true,result:{success:false,
//      compile_clean:false, error_count:1, diagnostics:[{file,line,severity,
//      message}]}} for source_compile; assert tools/call returns isError:false
//      (a failed C++ compile is a NORMAL result, NOT a transport failure — the
//      diagnostics ride through as data). This pins the "success:false is
//      data" contract at the stdio layer for the C++ loop (the same contract
//      P6.6 pinned for Blueprint compiles).
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
// Adapted from this repo's scripts/p6-parity-smoke.mjs (adapt fidelity).
// Intentional delta: the tool under test is the C++ source compile loop
// (source_update + source_compile) rather than the Blueprint compile loop
// (blueprint_spawn + blueprint_compile). The canonical update body is the
// write-identity {path,mode,bytes_written,lines_written}; the canonical
// compile body is the structured UBT report {method,target,success,
// compile_clean,error_count,warning_count,diagnostics[]} with success SPLIT
// from compile_clean (a loaded editor holds the module DLL, so success:false
// + compile_clean:true is expected). The tool-error case pins path_escapes_jail
// (the P7.1 Source/ jail contract) instead of not_compiled.

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

// Canonical source_update result body the bridge emits — the write identity
// the agent chains from. The stub wraps this in {ok:true,result:<body>}; the
// MCP text block carries the INNER object after LiveClient.postTool unwraps
// it. This is the parity pin on the update identity field set ({path,mode,
// bytes_written,lines_written}).
const SOURCE_UPDATE_OK = {
  path: "MyGame/McpSmoke.cpp",
  mode: "full",
  bytes_written: 42,
  lines_written: 3,
};

// Canonical source_compile CLEAN result body — a UBT report with success:true
// + compile_clean:true + empty diagnostics on an ok:true envelope. The
// method:'ubt' tag distinguishes the deterministic build path from Live
// Coding.
const SOURCE_COMPILE_CLEAN = {
  method: "ubt",
  target: "MyGameEditor",
  configuration: "Development",
  platform: "Win64",
  return_code: 0,
  success: true,
  compile_clean: true,
  error_count: 0,
  warning_count: 0,
  diagnostics: [],
};

// Compile-FAILED data envelope — the P7.3 contract: success:false on an
// ok:true envelope. The bridge ran the compile, the compiler reported errors,
// but the dispatch succeeded — so MCP isError MUST stay false and the
// diagnostics ride through as data, NOT as a transport error. compile_clean
// is false (a compiler error was emitted); the diagnostics[] carry file/line.
const SOURCE_COMPILE_FAILED = {
  ok: true,
  result: {
    method: "ubt",
    target: "MyGameEditor",
    configuration: "Development",
    platform: "Win64",
    return_code: 1,
    success: false,
    compile_clean: false,
    error_count: 1,
    warning_count: 0,
    diagnostics: [
      {
        file: "MyGame/McpSmoke.cpp",
        line: 7,
        severity: "error",
        message: "expected a ';'",
      },
    ],
  },
};

// Tool-error envelope the bridge emits when the handler ran but returned a
// structured failure — here a Source/ jail escape (an agent passed ../ or an
// absolute path outside the project's Source/). The {ok:false,error:{code,
// message}} body surfaces as an MCP error carrying the tool-specific
// path_escapes_jail code so an agent can branch on the cause (rewrite the
// path inside Source/) instead of retrying blindly.
const SOURCE_UPDATE_JAIL_ESCAPE = {
  ok: false,
  error: {
    code: "path_escapes_jail",
    message:
      "Path '../Engine/Source/Runtime/Core/Private/Core.cpp' escapes the project Source/ jail.",
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
  console.error(`Usage: node scripts/p7-parity-smoke.mjs [options]

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

/** Handler that serves GET /ping + POST source_update AND source_compile
 *  with their canonical ok:true envelopes (the healthy-loop case). */
async function healthySourceHandler(req, res) {
  if (req.method === "GET" && req.url === "/ping") {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(HEALTHY_PING));
    return;
  }
  if (
    req.method === "POST" &&
    req.url === "/tools/unreal_open_mcp_source_update"
  ) {
    await readBody(req);
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ ok: true, result: SOURCE_UPDATE_OK }));
    return;
  }
  if (
    req.method === "POST" &&
    req.url === "/tools/unreal_open_mcp_source_compile"
  ) {
    await readBody(req);
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ ok: true, result: SOURCE_COMPILE_CLEAN }));
    return;
  }
  res.writeHead(404, { "Content-Type": "application/json" });
  res.end(
    JSON.stringify({
      error: { code: "not_found", message: `${req.method} ${req.url}` },
    }),
  );
}

/** Handler that serves POST source_update → ok:false error envelope
 *  (the path_escapes_jail tool-error case). */
async function sourceUpdateErrorHandler(req, res) {
  if (
    req.method === "POST" &&
    req.url === "/tools/unreal_open_mcp_source_update"
  ) {
    await readBody(req);
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(SOURCE_UPDATE_JAIL_ESCAPE));
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

/** Handler that serves POST source_compile → success:false data envelope
 *  (the P7.3 contract case: compile failure is data, not a transport error). */
async function compileFailedHandler(req, res) {
  if (req.method === "GET" && req.url === "/ping") {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(HEALTHY_PING));
    return;
  }
  if (
    req.method === "POST" &&
    req.url === "/tools/unreal_open_mcp_source_compile"
  ) {
    await readBody(req);
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify(SOURCE_COMPILE_FAILED));
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
 * The `tool` arg selects which source tool the smoke drives, and `args`
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
        clientInfo: { name: "p7-smoke", version: "0.0.0" },
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
  // smoke (ping for transport parity, source_update + source_compile as the
  // compile-loop tools under test). The full-set pin lives in
  // integration.test.ts; here we only need the smoke's tools present.
  check(
    "tools/list advertises unreal_open_mcp_ping",
    tools.includes("unreal_open_mcp_ping"),
    `got [${tools.join(", ")}]`,
  );
  check(
    "tools/list advertises unreal_open_mcp_source_update",
    tools.includes("unreal_open_mcp_source_update"),
    `got [${tools.join(", ")}]`,
  );
  check(
    "tools/list advertises unreal_open_mcp_source_compile",
    tools.includes("unreal_open_mcp_source_compile"),
    `got [${tools.join(", ")}]`,
  );

  return { messages, ok: true };
}

// ---------------------------------------------------------------------------
// cases
// ---------------------------------------------------------------------------

async function caseHealthyUpdate(opts) {
  console.log("\nCase 1a: healthy source_update round-trip (stub → ok:true)");
  const stub = await startBridgeStub(0, healthySourceHandler);
  console.log(`  bridge:  stub on 127.0.0.1:${stub.port}`);
  try {
    const outcome = await runServer({
      projectPath: opts.project,
      bridgePort: stub.port,
      tool: "unreal_open_mcp_source_update",
      args: {
        path: "MyGame/McpSmoke.cpp",
        content: "// mcp smoke\nint32 main() { return 0; }\n",
        paths_hint: ["MyGame/McpSmoke.cpp"],
      },
    });
    if (outcome.error) {
      check("healthy update case drove the server", false, outcome.error);
      return;
    }
    const { messages } = assertCommonInit(outcome);
    if (!messages.length) return;

    const call = findById(messages, 3);
    check(
      "tools/call source_update returns isError:false",
      call?.result?.isError === false,
      `isError=${JSON.stringify(call?.result?.isError)}`,
    );

    const body = bodyOf(call);
    check(
      "source_update body is valid JSON",
      body !== undefined,
      "first content block was not a JSON text block",
    );
    if (body !== undefined) {
      // The INNER result object (not the {ok,result} envelope) must survive
      // the round-trip verbatim — the parity pin on the update identity field
      // set.
      check(
        "source_update body matches the stub result exactly (envelope unwrapped)",
        JSON.stringify(body) === JSON.stringify(SOURCE_UPDATE_OK),
        `got ${JSON.stringify(body)}`,
      );
    }
  } finally {
    await stub.close();
  }
}

async function caseHealthyCompile(opts) {
  console.log("\nCase 1b: healthy source_compile round-trip (stub → ok:true clean UBT report)");
  const stub = await startBridgeStub(0, healthySourceHandler);
  console.log(`  bridge:  stub on 127.0.0.1:${stub.port}`);
  try {
    const outcome = await runServer({
      projectPath: opts.project,
      bridgePort: stub.port,
      tool: "unreal_open_mcp_source_compile",
      args: {
        use_live_coding: false,
        paths_hint: ["MyGame/McpSmoke.cpp"],
      },
    });
    if (outcome.error) {
      check("healthy compile case drove the server", false, outcome.error);
      return;
    }
    const { messages } = assertCommonInit(outcome);
    if (!messages.length) return;

    const call = findById(messages, 3);
    check(
      "tools/call source_compile returns isError:false",
      call?.result?.isError === false,
      `isError=${JSON.stringify(call?.result?.isError)}`,
    );

    const body = bodyOf(call);
    check(
      "source_compile body is valid JSON",
      body !== undefined,
      "first content block was not a JSON text block",
    );
    if (body !== undefined) {
      // The structured UBT report (method:'ubt', success:true,
      // compile_clean:true, error_count:0, diagnostics:[]) must survive the
      // round-trip verbatim.
      check(
        "source_compile body matches the clean UBT report exactly (envelope unwrapped)",
        JSON.stringify(body) === JSON.stringify(SOURCE_COMPILE_CLEAN),
        `got ${JSON.stringify(body)}`,
      );
    }
  } finally {
    await stub.close();
  }
}

async function caseBridgeDown(opts) {
  console.log("\nCase 2: bridge-down source_update surfaces bridge_offline (dead port)");
  // Pin to port 1 — nothing listening, ECONNREFUSED. The server starts fine
  // (no preflight ping); tools/list is local; only tools/call(source_update)
  // hits the dead bridge and must classify as bridge_offline with the
  // instance-lock hint.
  const outcome = await runServer({
    projectPath: opts.project,
    bridgePort: 1,
    tool: "unreal_open_mcp_source_update",
    args: {
      path: "MyGame/McpSmoke.cpp",
      content: "// mcp smoke\n",
      paths_hint: ["MyGame/McpSmoke.cpp"],
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
    "tools/call source_update returns isError:true",
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
  console.log("\nCase 3: tool-error source_update surfaces the ok:false envelope (stub → ok:false, path_escapes_jail)");
  const stub = await startBridgeStub(0, sourceUpdateErrorHandler);
  console.log(`  bridge:  stub on 127.0.0.1:${stub.port}`);
  try {
    const outcome = await runServer({
      projectPath: opts.project,
      bridgePort: stub.port,
      tool: "unreal_open_mcp_source_update",
      args: {
        path: "../Engine/Source/Runtime/Core/Private/Core.cpp",
        content: "// should be rejected\n",
        paths_hint: ["../Engine/Source/Runtime/Core/Private/Core.cpp"],
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
      "tools/call source_update returns isError:true",
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
      // path_escapes_jail (rewrite the path inside Source/) rather than seeing
      // an opaque transport error.
      check(
        "tool-error code is path_escapes_jail (tool-specific, not transport)",
        body?.error?.code === "path_escapes_jail",
        `code=${JSON.stringify(body?.error?.code)}`,
      );
      check(
        "tool-error message rides through verbatim",
        body?.error?.message === SOURCE_UPDATE_JAIL_ESCAPE.error.message,
        `message=${JSON.stringify(body?.error?.message)}`,
      );
    }
  } finally {
    await stub.close();
  }
}

async function caseCompileFailedData(opts) {
  console.log("\nCase 4: compile success:false stays isError:false (compile failure is data, not transport)");
  const stub = await startBridgeStub(0, compileFailedHandler);
  console.log(`  bridge:  stub on 127.0.0.1:${stub.port}`);
  try {
    const outcome = await runServer({
      projectPath: opts.project,
      bridgePort: stub.port,
      tool: "unreal_open_mcp_source_compile",
      args: {
        use_live_coding: false,
        paths_hint: ["MyGame/McpSmoke.cpp"],
      },
    });
    if (outcome.error) {
      check("compile-failed case drove the server", false, outcome.error);
      return;
    }
    const { messages } = assertCommonInit(outcome);
    if (!messages.length) return;

    const call = findById(messages, 3);
    // CRITICAL P7.3 invariant: a failed compile is a NORMAL result, NOT a
    // transport failure. The envelope stays ok:true with success:false +
    // compile_clean:false + a populated diagnostics[], so MCP isError MUST
    // stay false and the diagnostics ride through as data.
    check(
      "tools/call source_compile with success:false returns isError:false (data, not error)",
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
        "compile-failed body carries success:false",
        body?.success === false,
        `success=${JSON.stringify(body?.success)}`,
      );
      check(
        "compile-failed body carries compile_clean:false",
        body?.compile_clean === false,
        `compile_clean=${JSON.stringify(body?.compile_clean)}`,
      );
      check(
        "compile-failed body carries the error count",
        body?.error_count === 1,
        `error_count=${JSON.stringify(body?.error_count)}`,
      );
      check(
        "compile-failed body carries the diagnostics[] (file/line/severity/message)",
        Array.isArray(body?.diagnostics) &&
          body.diagnostics.length === 1 &&
          body.diagnostics[0].severity === "error" &&
          typeof body.diagnostics[0].file === "string" &&
          typeof body.diagnostics[0].line === "number" &&
          typeof body.diagnostics[0].message === "string",
        `diagnostics=${JSON.stringify(body?.diagnostics).slice(0, 200)}`,
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

  console.log("unreal-open-mcp — Source compile-loop parity smoke");
  console.log(`  server:  ${SERVER_ENTRY}`);
  console.log(`  project: ${opts.project}`);

  if (!existsSync(SERVER_ENTRY)) {
    console.error(`\nBuilt server not found at ${SERVER_ENTRY}.`);
    console.error("Run `npm run build` in mcp-server/ first.");
    process.exit(2);
  }

  // The live-editor path (--port) only exercises the healthy update case
  // meaningfully; the other three cases need a controllable stub. When --port
  // is set we still run the healthy update against the live bridge (shape
  // check only — the body won't match SOURCE_UPDATE_OK) and skip the rest.
  if (opts.port !== null) {
    console.log(`\n--port ${opts.port}: running healthy update case against live bridge only.`);
    console.log("  (bridge-down + tool-error + compile-failed cases require the stub harness.)");
    await caseHealthyUpdateLive(opts);
  } else {
    await caseHealthyUpdate(opts);
    await caseHealthyCompile(opts);
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
 * Live-editor variant of the healthy update case (--port). Cannot assert the
 * exact body — only that the round-trip returns isError:false with a
 * recognizable update shape (path + mode + bytes_written + lines_written).
 */
async function caseHealthyUpdateLive(opts) {
  console.log("\nCase 1 (live): healthy source_update round-trip (live bridge)");
  console.log(`  bridge:  live on 127.0.0.1:${opts.port}`);
  const outcome = await runServer({
    projectPath: opts.project,
    bridgePort: opts.port,
    tool: "unreal_open_mcp_source_update",
    args: {
      path: "MyGame/McpSmoke.cpp",
      content: "// mcp smoke\n",
      paths_hint: ["MyGame/McpSmoke.cpp"],
    },
  });
  if (outcome.error) {
    check("live healthy update case drove the server", false, outcome.error);
    return;
  }
  const { messages } = assertCommonInit(outcome);
  if (!messages.length) return;

  const call = findById(messages, 3);
  check(
    "tools/call source_update returns isError:false",
    call?.result?.isError === false,
    `isError=${JSON.stringify(call?.result?.isError)}`,
  );
  const body = bodyOf(call);
  if (body !== undefined) {
    check(
      "source_update body carries the path + mode + bytes_written shape",
      typeof body?.path === "string" &&
        typeof body?.mode === "string" &&
        typeof body?.bytes_written === "number" &&
        typeof body?.lines_written === "number",
      `got ${JSON.stringify(body).slice(0, 160)}`,
    );
  }
}

main().catch((err) => {
  console.error("p7-parity-smoke fatal:", err);
  process.exit(1);
});
