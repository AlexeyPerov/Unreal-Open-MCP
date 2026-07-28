// Tests for `setup-mcp` (lib/setup-mcp.ts + lib/agents.ts +
// commands/setup-mcp.ts). Temp-dir based — no editor, no network, no real
// project required.
//
// Built + run via the package test config (see package.json `test`):
//   tsc -p tsconfig.test.json  &&  node --test 'dist-test/**/*.test.js'

import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import * as os from "node:os";

import {
  MCP_SERVER_PACKAGE,
  buildServerEntry,
  mergeJsonAgentConfig,
  resolveServerCommand,
  setupMcp,
} from "./setup-mcp.js";
import {
  MCP_SERVER_NAME,
  agentRegistry,
  getAgentById,
  getAgentIds,
} from "./agents.js";
import {
  formatAgentList,
  formatHuman,
  formatJson,
  resolveProjectDir,
  runSetupMcpCommand,
} from "../commands/setup-mcp.js";

// ---------------------------------------------------------------------------
// agents.ts
// ---------------------------------------------------------------------------

test("agentRegistry: cursor + claude are present (acceptance-criteria agents)", () => {
  const ids = getAgentIds();
  assert.ok(ids.includes("cursor"));
  assert.ok(ids.includes("claude"));
});

test("getAgentById: returns the matching agent", () => {
  const a = getAgentById("cursor");
  assert.ok(a);
  assert.equal(a?.id, "cursor");
  assert.equal(a?.bodyPath, "mcpServers");
});

test("getAgentById: undefined for unknown id", () => {
  assert.equal(getAgentById("nope"), undefined);
});

test("cursor agent resolves a project-local config path", () => {
  const a = getAgentById("cursor");
  assert.ok(a);
  assert.equal(a?.getConfigPath("/p/MyGame"), path.join("/p/MyGame", ".cursor", "mcp.json"));
});

test("MCP_SERVER_NAME is 'unreal-open-mcp' (Unreal-specific delta vs Unity)", () => {
  assert.equal(MCP_SERVER_NAME, "unreal-open-mcp");
});

// ---------------------------------------------------------------------------
// resolveServerCommand
// ---------------------------------------------------------------------------

test("resolveServerCommand: default vector is npx -y unreal-open-mcp@<cliVersion>", () => {
  const v = resolveServerCommand({}, "1.2.3");
  assert.equal(v.command, "npx");
  assert.deepEqual(v.args, ["-y", `${MCP_SERVER_PACKAGE}@1.2.3`]);
});

test("resolveServerCommand: falls back to 'latest' when cliVersion is empty", () => {
  const v = resolveServerCommand({}, "");
  assert.deepEqual(v.args, ["-y", `${MCP_SERVER_PACKAGE}@latest`]);
});

test("resolveServerCommand: --server-command node resolves the monorepo server entry", () => {
  const fakeModuleUrl = `file://${path.join(os.tmpdir(), "fake-cli", "dist", "lib", "setup-mcp.js")}`;
  const v = resolveServerCommand({ serverCommand: "node" }, "1.2.3", fakeModuleUrl);
  assert.equal(v.command, "node");
  assert.equal(v.args.length, 1);
  // path resolves up from cli/dist/lib/ → ../../mcp-server/dist/index.js
  assert.match(v.args[0], /mcp-server.*dist.*index\.js$/);
});

test("resolveServerCommand: bare command override emits the command with no args", () => {
  const v = resolveServerCommand({ serverCommand: "unreal-open-mcp" }, "1.2.3");
  assert.equal(v.command, "unreal-open-mcp");
  assert.deepEqual(v.args, []);
});

test("resolveServerCommand: trims whitespace in the override", () => {
  const v = resolveServerCommand({ serverCommand: "  unreal-open-mcp  " }, "1.2.3");
  assert.equal(v.command, "unreal-open-mcp");
});

// ---------------------------------------------------------------------------
// buildServerEntry
// ---------------------------------------------------------------------------

test("buildServerEntry: stdio shape with command + args + env.UNREAL_PROJECT_PATH", () => {
  const entry = buildServerEntry(
    "/abs/MyGame",
    { command: "npx", args: ["-y", "unreal-open-mcp@1.2.3"] },
  );
  assert.equal(entry.command, "npx");
  assert.deepEqual(entry.args, ["-y", "unreal-open-mcp@1.2.3"]);
  assert.deepEqual(entry.env, { UNREAL_PROJECT_PATH: "/abs/MyGame" });
});

test("buildServerEntry: omits the port env var when bridgePort is not set", () => {
  const entry = buildServerEntry(
    "/abs/MyGame",
    { command: "npx", args: [] },
  );
  assert.ok(!("UNREAL_OPEN_MCP_BRIDGE_PORT" in (entry.env as Record<string, string>)));
});

test("buildServerEntry: includes the port env var only when bridgePort is set", () => {
  const entry = buildServerEntry(
    "/abs/MyGame",
    { command: "npx", args: [] },
    23456,
  );
  assert.deepEqual(entry.env, {
    UNREAL_PROJECT_PATH: "/abs/MyGame",
    UNREAL_OPEN_MCP_BRIDGE_PORT: "23456",
  });
});

// ---------------------------------------------------------------------------
// mergeJsonAgentConfig
// ---------------------------------------------------------------------------

test("mergeJsonAgentConfig: writes a fresh config when none exists", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-merge-"));
  try {
    const cfg = path.join(tmp, ".cursor", "mcp.json");
    const entry = buildServerEntry("/abs/MyGame", { command: "npx", args: [] });
    const { content, warning } = mergeJsonAgentConfig(cfg, "mcpServers", MCP_SERVER_NAME, entry, false);
    assert.equal(warning, undefined);
    assert.ok(fs.existsSync(cfg), "config file was created (with parent dir)");

    const parsed = JSON.parse(content);
    assert.deepEqual(parsed.mcpServers[MCP_SERVER_NAME], entry);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("mergeJsonAgentConfig: dry-run returns content but writes nothing", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-drymerge-"));
  try {
    const cfg = path.join(tmp, ".cursor", "mcp.json");
    const entry = buildServerEntry("/abs/MyGame", { command: "npx", args: [] });
    const { content } = mergeJsonAgentConfig(cfg, "mcpServers", MCP_SERVER_NAME, entry, true);
    assert.ok(!fs.existsSync(cfg), "dry-run must not create the file");
    const parsed = JSON.parse(content);
    assert.deepEqual(parsed.mcpServers[MCP_SERVER_NAME], entry);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("mergeJsonAgentConfig: preserves sibling servers + unrelated top-level keys", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-sibling-"));
  try {
    const cfg = path.join(tmp, ".cursor", "mcp.json");
    // existing config has an unrelated server + a top-level schema key
    const pre = {
      "$schema": "https://example.com/schema.json",
      mcpServers: {
        "other-server": { command: "echo", args: ["hi"] },
      },
    };
    fs.mkdirSync(path.dirname(cfg), { recursive: true });
    fs.writeFileSync(cfg, JSON.stringify(pre, null, 2) + "\n");

    const entry = buildServerEntry("/abs/MyGame", { command: "npx", args: [] });
    const { content, warning } = mergeJsonAgentConfig(cfg, "mcpServers", MCP_SERVER_NAME, entry, false);
    assert.equal(warning, undefined);

    const parsed = JSON.parse(content);
    // unrelated top-level key preserved
    assert.equal(parsed.$schema, "https://example.com/schema.json");
    // sibling server preserved
    assert.deepEqual(parsed.mcpServers["other-server"], { command: "echo", args: ["hi"] });
    // our entry merged in
    assert.deepEqual(parsed.mcpServers[MCP_SERVER_NAME], entry);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("mergeJsonAgentConfig: replaces the prior unreal-open-mcp entry (idempotent re-run)", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-rerun-"));
  try {
    const cfg = path.join(tmp, ".cursor", "mcp.json");
    fs.mkdirSync(path.dirname(cfg), { recursive: true });
    // prior entry has a stale transport field (`url`) the new stdio entry must drop
    const pre = {
      mcpServers: {
        [MCP_SERVER_NAME]: { command: "old", args: [], url: "http://stale" },
      },
    };
    fs.writeFileSync(cfg, JSON.stringify(pre, null, 2) + "\n");

    const entry = buildServerEntry("/abs/MyGame", { command: "npx", args: ["-y", "unreal-open-mcp@2.0.0"] });
    const { content } = mergeJsonAgentConfig(cfg, "mcpServers", MCP_SERVER_NAME, entry, false);
    const parsed = JSON.parse(content);

    // exactly one entry under our key (no duplicate)
    assert.deepEqual(parsed.mcpServers[MCP_SERVER_NAME], entry);
    assert.ok(!("url" in parsed.mcpServers[MCP_SERVER_NAME]), "stale transport key dropped");
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("mergeJsonAgentConfig: malformed existing JSON → warning + start fresh", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-badmerge-"));
  try {
    const cfg = path.join(tmp, ".cursor", "mcp.json");
    fs.mkdirSync(path.dirname(cfg), { recursive: true });
    fs.writeFileSync(cfg, "{ not json");

    const entry = buildServerEntry("/abs/MyGame", { command: "npx", args: [] });
    const { content, warning } = mergeJsonAgentConfig(cfg, "mcpServers", MCP_SERVER_NAME, entry, false);
    assert.ok(warning, "malformed JSON must surface a warning");
    assert.match(warning!, /malformed JSON/);

    const parsed = JSON.parse(content);
    assert.deepEqual(parsed.mcpServers[MCP_SERVER_NAME], entry);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

// ---------------------------------------------------------------------------
// setupMcp (lib) — happy paths + error paths
// ---------------------------------------------------------------------------

test("setupMcp: cursor writes a valid stdio snippet with the project path", async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-cursor-"));
  try {
    const r = await setupMcp({ agentId: "cursor", projectDir: tmp }, "0.4.2");
    assert.equal(r.ok, true);
    if (!r.ok) return;
    assert.equal(r.written, true);
    assert.equal(r.agentId, "cursor");
    assert.equal(r.configPath, path.join(tmp, ".cursor", "mcp.json"));

    // snippet carries stdio + absolute project path + version-pinned package
    assert.match(r.snippet, /"command": "npx"/);
    assert.match(r.snippet, /unreal-open-mcp@0\.4\.2/);
    assert.match(r.snippet, new RegExp(`"UNREAL_PROJECT_PATH": "${tmp}"`));
    // port env var not invented
    assert.ok(!/UNREAL_OPEN_MCP_BRIDGE_PORT/.test(r.snippet));

    // file was actually written
    assert.ok(fs.existsSync(r.configPath));
    const parsed = JSON.parse(fs.readFileSync(r.configPath, "utf8"));
    assert.deepEqual(parsed.mcpServers[MCP_SERVER_NAME].env, { UNREAL_PROJECT_PATH: tmp });

    // next-steps mention restart + install-plugin
    assert.ok(r.nextSteps.some((s) => /Restart Cursor/.test(s)));
    assert.ok(r.nextSteps.some((s) => /install-plugin/.test(s)));
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("setupMcp: claude writes to the OS-global Claude Desktop config path", async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-claude-"));
  try {
    const r = await setupMcp({ agentId: "claude", projectDir: tmp });
    assert.equal(r.ok, true);
    if (!r.ok) return;
    // config path is OS-global (ignores projectDir)
    assert.ok(!r.configPath.startsWith(tmp), "claude config path must NOT live under the project dir");
    assert.match(r.configPath, /claude_desktop_config\.json$/);
    // clean up the written file (Claude config is OS-global)
    if (fs.existsSync(r.configPath)) {
      // only remove the file, NOT the user's real Claude dir tree
      fs.rmSync(r.configPath, { force: true });
    }
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("setupMcp: --dry-run returns snippet without writing", async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-dry-"));
  try {
    const r = await setupMcp({ agentId: "cursor", projectDir: tmp, dryRun: true }, "0.4.2");
    assert.equal(r.ok, true);
    if (!r.ok) return;
    assert.equal(r.written, false);
    assert.ok(!fs.existsSync(r.configPath), "dry-run must not write the file");
    assert.match(r.snippet, /"command": "npx"/);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("setupMcp: bridgePort adds the port env var", async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-port-"));
  try {
    const r = await setupMcp({ agentId: "cursor", projectDir: tmp, bridgePort: 23456 });
    assert.equal(r.ok, true);
    if (!r.ok) return;
    assert.match(r.snippet, /"UNREAL_OPEN_MCP_BRIDGE_PORT": "23456"/);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("setupMcp: unknown agent → unknown_agent", async () => {
  const r = await setupMcp({ agentId: "bogus", projectDir: "/abs/MyGame" });
  assert.equal(r.ok, false);
  if (r.ok) return;
  assert.equal(r.kind, "unknown_agent");
});

test("setupMcp: relative projectDir → project_dir_required", async () => {
  const r = await setupMcp({ agentId: "cursor", projectDir: "relative/MyGame" });
  assert.equal(r.ok, false);
  if (r.ok) return;
  assert.equal(r.kind, "project_dir_required");
});

test("setupMcp: merges into an existing config without clobbering siblings", async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-clobber-"));
  try {
    const cfg = path.join(tmp, ".cursor", "mcp.json");
    fs.mkdirSync(path.dirname(cfg), { recursive: true });
    fs.writeFileSync(cfg, JSON.stringify({
      mcpServers: { "keep-me": { command: "echo", args: [] } },
    }, null, 2) + "\n");

    const r = await setupMcp({ agentId: "cursor", projectDir: tmp });
    assert.equal(r.ok, true);
    if (!r.ok) return;
    const parsed = JSON.parse(fs.readFileSync(cfg, "utf8"));
    assert.ok(parsed.mcpServers["keep-me"], "sibling server preserved");
    assert.ok(parsed.mcpServers[MCP_SERVER_NAME], "our server merged in");
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

// ---------------------------------------------------------------------------
// commands/setup-mcp.ts
// ---------------------------------------------------------------------------

test("resolveProjectDir: --project wins over env / cwd", () => {
  const got = resolveProjectDir({
    projectPath: "/flag",
    cwd: "/cwd",
    env: { UNREAL_PROJECT_PATH: "/env" },
  });
  assert.equal(got, "/flag");
});

test("resolveProjectDir: env wins over cwd", () => {
  const got = resolveProjectDir({ cwd: "/cwd", env: { UNREAL_PROJECT_PATH: "/env" } });
  assert.equal(got, "/env");
});

test("resolveProjectDir: falls back to cwd", () => {
  const got = resolveProjectDir({ cwd: "/cwd", env: {} });
  assert.equal(got, "/cwd");
});

test("resolveProjectDir: absolutizes a relative --project path", () => {
  const got = resolveProjectDir({ projectPath: "relative", cwd: "/cwd", env: {} });
  assert.ok(path.isAbsolute(got), "resolved dir must be absolute");
  assert.equal(got, path.resolve("/cwd", "relative"));
});

test("formatAgentList: lists every registry id", () => {
  const text = formatAgentList();
  for (const a of agentRegistry) {
    assert.ok(text.includes(a.id), `list mentions ${a.id}`);
  }
  assert.match(text, /Supported MCP agents/);
});

test("formatHuman: success block lists snippet + next steps", async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-fmt-"));
  try {
    const r = await setupMcp({ agentId: "cursor", projectDir: tmp, dryRun: true });
    assert.equal(r.ok, true);
    if (!r.ok) return;
    const block = formatHuman(r, "unreal-open-mcp-cli");
    assert.match(block, /\[dry-run\]/);
    assert.match(block, /Snippet:/);
    assert.match(block, /Next:/);
    assert.match(block, /Restart Cursor/);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("formatHuman: failure block is a single-line error", () => {
  const block = formatHuman(
    { ok: false, kind: "unknown_agent", message: 'Unknown agent "bogus".' },
    "unreal-open-mcp-cli",
  );
  assert.match(block, /setup-mcp failed/);
  assert.match(block, /Unknown agent/);
});

test("formatJson: round-trips a success result as JSON", async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-fmtjson-"));
  try {
    const r = await setupMcp({ agentId: "cursor", projectDir: tmp, dryRun: true });
    assert.equal(r.ok, true);
    if (!r.ok) return;
    const body = formatJson(r);
    const parsed = JSON.parse(body);
    assert.equal(parsed.ok, true);
    assert.equal(parsed.agentId, "cursor");
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("runSetupMcpCommand: --list prints the agent table and exits 0", async () => {
  const outChunks: string[] = [];
  const errChunks: string[] = [];
  const outcome = await runSetupMcpCommand(
    { list: true },
    async (s) => { outChunks.push(s); },
    async (s) => { errChunks.push(s); },
    "unreal-open-mcp-cli",
  );
  assert.equal(outcome.exitCode, 0);
  assert.equal(errChunks.join(""), "");
  assert.match(outChunks.join(""), /Supported MCP agents/);
});

test("runSetupMcpCommand: --list --json emits a JSON agent roster", async () => {
  const outChunks: string[] = [];
  const outcome = await runSetupMcpCommand(
    { list: true, json: true },
    async (s) => { outChunks.push(s); },
    async () => {},
    "unreal-open-mcp-cli",
  );
  assert.equal(outcome.exitCode, 0);
  const parsed = JSON.parse(outChunks.join(""));
  assert.ok(Array.isArray(parsed.agents));
  assert.ok(parsed.agents.some((a: { id: string }) => a.id === "cursor"));
});

test("runSetupMcpCommand: missing agent → exits 2 with a helpful message", async () => {
  const outChunks: string[] = [];
  const errChunks: string[] = [];
  const outcome = await runSetupMcpCommand(
    {},
    async (s) => { outChunks.push(s); },
    async (s) => { errChunks.push(s); },
    "unreal-open-mcp-cli",
  );
  assert.equal(outcome.exitCode, 2);
  assert.equal(outChunks.join(""), "");
  assert.match(errChunks.join(""), /requires an agent id/);
  assert.match(errChunks.join(""), /setup-mcp --list/);
});

test("runSetupMcpCommand: unknown agent → exits 2 with the failure envelope", async () => {
  const outChunks: string[] = [];
  const errChunks: string[] = [];
  const outcome = await runSetupMcpCommand(
    { positionalAgent: "bogus", projectPath: "/abs/MyGame" },
    async (s) => { outChunks.push(s); },
    async (s) => { errChunks.push(s); },
    "unreal-open-mcp-cli",
  );
  assert.equal(outcome.exitCode, 2);
  assert.equal(outChunks.join(""), "");
  assert.match(errChunks.join(""), /setup-mcp failed/);
});

test("runSetupMcpCommand: end-to-end cursor dry-run prints the snippet (human)", async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-e2e-"));
  const outChunks: string[] = [];
  const errChunks: string[] = [];
  try {
    const outcome = await runSetupMcpCommand(
      {
        positionalAgent: "cursor",
        projectPath: tmp,
        dryRun: true,
      },
      async (s) => { outChunks.push(s); },
      async (s) => { errChunks.push(s); },
      "unreal-open-mcp-cli",
    );
    assert.equal(outcome.exitCode, 0);
    assert.equal(errChunks.join(""), "");
    const joined = outChunks.join("");
    assert.match(joined, /\[dry-run\]/);
    assert.match(joined, /"command": "npx"/);
    // the snippet must carry the absolute project path under UNREAL_PROJECT_PATH
    assert.ok(
      joined.includes(`"UNREAL_PROJECT_PATH": "${tmp}"`),
      "snippet must embed the absolute project path",
    );
    assert.ok(!fs.existsSync(path.join(tmp, ".cursor", "mcp.json")), "dry-run wrote nothing");
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("runSetupMcpCommand: --json success emits the JSON envelope on stdout", async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-json-"));
  const outChunks: string[] = [];
  const errChunks: string[] = [];
  try {
    const outcome = await runSetupMcpCommand(
      {
        positionalAgent: "cursor",
        projectPath: tmp,
        dryRun: true,
        json: true,
      },
      async (s) => { outChunks.push(s); },
      async (s) => { errChunks.push(s); },
      "unreal-open-mcp-cli",
    );
    assert.equal(outcome.exitCode, 0);
    assert.equal(errChunks.join(""), "");
    const parsed = JSON.parse(outChunks.join(""));
    assert.equal(parsed.ok, true);
    assert.equal(parsed.agentId, "cursor");
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});
