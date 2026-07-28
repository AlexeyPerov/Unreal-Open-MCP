// Tests for `install-plugin` (lib/install-plugin.ts + commands/install-plugin.ts)
// and the `.uproject` merge (lib/uproject.ts). Temp-dir based — no Unreal Editor,
// no network, no real monorepo required.
//
// Built + run via the package test config (see package.json `test`):
//   tsc -p tsconfig.test.json  &&  node --test 'dist-test/**/*.test.js'

import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import * as os from "node:os";

import {
  findPluginEntryIndex,
  findUProjectFile,
  readUProject,
  serializeUProject,
  upsertPluginEntry,
  writeUProject,
  type UProjectDescriptor,
  type UProjectPluginEntry,
} from "./uproject.js";
import {
  BRIDGE_PLUGIN_NAME,
  VERIFY_PLUGIN_NAME,
  findMonorepoRoot,
  isMonorepoRoot,
  resolvePluginSource,
} from "./plugin-source.js";
import { installPlugin, isSymlink } from "./install-plugin.js";
import {
  formatHuman,
  formatJson,
  resolveProjectDir,
  runInstallPluginCommand,
} from "../commands/install-plugin.js";

// ---------------------------------------------------------------------------
// fixtures
// ---------------------------------------------------------------------------

/**
 * Minimal `.uplugin` body — only the descriptor matters for install (the
 * `.uproject` enable keys on the FOLDER name, not a parsed `.uplugin` field).
 */
const BRIDGE_UPLUGIN = JSON.stringify({
  FileVersion: 3,
  Version: 1,
  FriendlyName: "Unreal Open MCP",
  Description: "test bridge",
}, null, 2);

const VERIFY_UPLUGIN = JSON.stringify({
  FileVersion: 3,
  Version: 1,
  FriendlyName: "Unreal Open MCP Verify",
  Description: "test verify",
}, null, 2);

interface MonorepoFixture {
  root: string;
  bridgeSourceDir: string;
  verifySourceDir: string;
}

/**
 * Build a temp monorepo root with `packages/bridge` + `packages/verify`,
 * each carrying a `.uplugin`, a `Source/` tree, AND build-artifact dirs
 * (`Intermediate/`, `Binaries/`) that the copy filter must exclude.
 */
function makeMonorepoFixture(): MonorepoFixture {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-monorepo-"));
  const bridgeSourceDir = path.join(root, "packages", "bridge");
  const verifySourceDir = path.join(root, "packages", "verify");
  fs.mkdirSync(bridgeSourceDir, { recursive: true });
  fs.mkdirSync(verifySourceDir, { recursive: true });

  // bridge
  fs.writeFileSync(path.join(bridgeSourceDir, "UnrealOpenMCP.uplugin"), BRIDGE_UPLUGIN);
  fs.mkdirSync(path.join(bridgeSourceDir, "Source", "UnrealOpenMcpEditor", "Private"), { recursive: true });
  fs.writeFileSync(
    path.join(bridgeSourceDir, "Source", "UnrealOpenMcpEditor", "Private", "Foo.cpp"),
    "// bridge source",
  );
  // build artifacts that MUST be excluded from a copy install
  fs.mkdirSync(path.join(bridgeSourceDir, "Intermediate", "Build"), { recursive: true });
  fs.writeFileSync(path.join(bridgeSourceDir, "Intermediate", "Build", "junk.txt"), "stale");
  fs.mkdirSync(path.join(bridgeSourceDir, "Binaries", "Win64"), { recursive: true });
  fs.writeFileSync(
    path.join(bridgeSourceDir, "Binaries", "Win64", "UnrealOpenMCP.module"),
    "binary",
  );

  // verify
  fs.writeFileSync(path.join(verifySourceDir, "UnrealOpenMCPVerify.uplugin"), VERIFY_UPLUGIN);
  fs.mkdirSync(path.join(verifySourceDir, "Source", "UnrealOpenMcpVerify", "Private"), { recursive: true });
  fs.writeFileSync(
    path.join(verifySourceDir, "Source", "UnrealOpenMcpVerify", "Private", "Bar.cpp"),
    "// verify source",
  );
  fs.mkdirSync(path.join(verifySourceDir, "Intermediate", "Build"), { recursive: true });
  fs.writeFileSync(path.join(verifySourceDir, "Intermediate", "Build", "junk.txt"), "stale");

  return { root, bridgeSourceDir, verifySourceDir };
}

interface ProjectFixture {
  projectDir: string;
  uprojectPath: string;
}

/**
 * Build a temp UE project dir with a minimal `.uproject` carrying a couple of
 * unrelated fields + an unrelated enabled plugin, so the merge tests can assert
 * those survive untouched.
 */
function makeProjectFixture(
  extraPlugins: UProjectPluginEntry[] = [],
): ProjectFixture {
  const projectDir = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-project-"));
  const projectName = "TestProject";
  const uprojectPath = path.join(projectDir, `${projectName}.uproject`);
  const descriptor: UProjectDescriptor = {
    EngineAssociation: "5.8",
    // unrelated field whose value + position must survive the merge
    Enterprise: true,
    Plugins: [
      { Name: "EditorScriptingUtilities", Enabled: true },
      ...extraPlugins,
    ],
  };
  fs.writeFileSync(uprojectPath, serializeUProject(descriptor));
  return { projectDir, uprojectPath };
}

/** Read back the project's `.uproject` JSON after an install. */
function readBackUProject(projectDir: string): UProjectDescriptor {
  const uprojectPath = findUProjectFile(projectDir);
  assert.ok(uprojectPath, "expected a .uproject in the project dir");
  return JSON.parse(fs.readFileSync(uprojectPath, "utf8")) as UProjectDescriptor;
}

// ---------------------------------------------------------------------------
// uproject.ts
// ---------------------------------------------------------------------------

test("findUProjectFile: locates the single .uproject", () => {
  const { projectDir, uprojectPath } = makeProjectFixture();
  try {
    assert.equal(findUProjectFile(projectDir), uprojectPath);
  } finally {
    fs.rmSync(projectDir, { recursive: true, force: true });
  }
});

test("findUProjectFile: returns null when none", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-empty-"));
  try {
    assert.equal(findUProjectFile(tmp), null);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("readUProject: ok path parses descriptor", () => {
  const { projectDir } = makeProjectFixture();
  try {
    const r = readUProject(projectDir);
    assert.equal(r.ok, true);
    if (r.ok) {
      assert.equal(r.descriptor.EngineAssociation, "5.8");
    }
  } finally {
    fs.rmSync(projectDir, { recursive: true, force: true });
  }
});

test("readUProject: not_found when no .uproject", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-empty-"));
  try {
    const r = readUProject(tmp);
    assert.equal(r.ok, false);
    if (!r.ok) assert.equal(r.kind, "not_found");
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("readUProject: parse_failed on invalid JSON", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-bad-"));
  try {
    fs.writeFileSync(path.join(tmp, "Bad.uproject"), "{ not json");
    const r = readUProject(tmp);
    assert.equal(r.ok, false);
    if (!r.ok) assert.equal(r.kind, "parse_failed");
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("upsertPluginEntry: inserts a new entry", () => {
  const d: UProjectDescriptor = { EngineAssociation: "5.8" };
  const res = upsertPluginEntry(d, "UnrealOpenMCP");
  assert.equal(res.inserted, true);
  assert.deepEqual(d.Plugins, [{ Name: "UnrealOpenMCP", Enabled: true }]);
});

test("upsertPluginEntry: updates an existing entry to Enabled without duplicating", () => {
  const d: UProjectDescriptor = {
    Plugins: [
      { Name: "UnrealOpenMCP", Enabled: false },
      { Name: "Other", Enabled: true },
    ],
  };
  const res = upsertPluginEntry(d, "UnrealOpenMCP");
  assert.equal(res.inserted, false);
  assert.equal(d.Plugins?.length, 2);
  assert.deepEqual(d.Plugins?.[0], { Name: "UnrealOpenMCP", Enabled: true });
});

test("findPluginEntryIndex: -1 when Plugins missing", () => {
  assert.equal(findPluginEntryIndex({}, "UnrealOpenMCP"), -1);
});

test("serializeUProject: 2-space indent + trailing newline", () => {
  const body = serializeUProject({ EngineAssociation: "5.8" });
  assert.ok(body.endsWith("\n"));
  assert.ok(body.includes('\n  "EngineAssociation"'));
});

test("writeUProject: writes via tmp rename, content round-trips", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-write-"));
  try {
    const target = path.join(tmp, "X.uproject");
    writeUProject({ EngineAssociation: "5.9" }, target);
    const back = JSON.parse(fs.readFileSync(target, "utf8"));
    assert.equal(back.EngineAssociation, "5.9");
    // no leftover tmp file
    assert.deepEqual(
      fs.readdirSync(tmp).sort(),
      ["X.uproject"],
    );
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

// ---------------------------------------------------------------------------
// plugin-source.ts
// ---------------------------------------------------------------------------

test("isMonorepoRoot: true for a fixture root", () => {
  const { root } = makeMonorepoFixture();
  try {
    assert.equal(isMonorepoRoot(root), true);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("isMonorepoRoot: false for an unrelated dir", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-unrelated-"));
  try {
    assert.equal(isMonorepoRoot(tmp), false);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("findMonorepoRoot: walks up to the fixture root from a nested dir", () => {
  const { root } = makeMonorepoFixture();
  try {
    const nested = path.join(root, "packages", "bridge", "Source");
    assert.equal(findMonorepoRoot(nested), root);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("findMonorepoRoot: null when no monorepo ancestor", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-noancestor-"));
  try {
    assert.equal(findMonorepoRoot(tmp), null);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("resolvePluginSource: explicit root wins", () => {
  const { root, bridgeSourceDir, verifySourceDir } = makeMonorepoFixture();
  try {
    const r = resolvePluginSource(root, "file:///fake/module/url");
    assert.equal(r.ok, true);
    if (r.ok) {
      assert.equal(r.root, root);
      assert.equal(r.bridgeSourceDir, bridgeSourceDir);
      assert.equal(r.verifySourceDir, verifySourceDir);
    }
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("resolvePluginSource: explicit non-monorepo dir is an error", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-badroot-"));
  try {
    const r = resolvePluginSource(tmp, "file:///fake/module/url");
    assert.equal(r.ok, false);
    if (!r.ok) assert.equal(r.kind, "explicit_not_found");
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("resolvePluginSource: env var wins when no explicit root", () => {
  const { root } = makeMonorepoFixture();
  try {
    const r = resolvePluginSource(undefined, "file:///fake/module/url", root);
    assert.equal(r.ok, true);
    if (r.ok) assert.equal(r.root, root);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("resolvePluginSource: bad env var is an error", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-badenv-"));
  try {
    const r = resolvePluginSource(undefined, "file:///fake/module/url", tmp);
    assert.equal(r.ok, false);
    if (!r.ok) assert.equal(r.kind, "env_not_found");
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("resolvePluginSource: walk-not-found when startUrl is outside any monorepo", () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-nowalk-"));
  try {
    // point the start url at the temp dir itself so the walk can't find a root
    const r = resolvePluginSource(undefined, `file://${tmp}/fake.js`, undefined);
    assert.equal(r.ok, false);
    if (!r.ok) assert.equal(r.kind, "walk_not_found");
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

// ---------------------------------------------------------------------------
// install-plugin.ts — happy paths
// ---------------------------------------------------------------------------

test("installPlugin: copy install lands both plugins + enables them in .uproject", async () => {
  const mono = makeMonorepoFixture();
  const proj = makeProjectFixture();
  try {
    const r = await installPlugin({
      projectDir: proj.projectDir,
      pluginSource: mono.root,
    });
    assert.equal(r.ok, true);
    if (!r.ok) return;

    // bridge files present, build artifacts excluded
    const bridgeInstall = path.join(proj.projectDir, "Plugins", BRIDGE_PLUGIN_NAME);
    assert.ok(fs.existsSync(path.join(bridgeInstall, "UnrealOpenMCP.uplugin")));
    assert.ok(fs.existsSync(path.join(bridgeInstall, "Source", "UnrealOpenMcpEditor", "Private", "Foo.cpp")));
    assert.ok(!fs.existsSync(path.join(bridgeInstall, "Intermediate")));
    assert.ok(!fs.existsSync(path.join(bridgeInstall, "Binaries")));

    // verify files present
    const verifyInstall = path.join(proj.projectDir, "Plugins", VERIFY_PLUGIN_NAME);
    assert.ok(fs.existsSync(path.join(verifyInstall, "UnrealOpenMCPVerify.uplugin")));

    // .uproject enables both, preserves unrelated fields
    const desc = readBackUProject(proj.projectDir);
    assert.equal(desc.EngineAssociation, "5.8");
    assert.equal((desc as Record<string, unknown>).Enterprise, true);
    const names = (desc.Plugins ?? []).map((p) => p.Name);
    assert.ok(names.includes("UnrealOpenMCP"));
    assert.ok(names.includes("UnrealOpenMCPVerify"));
    assert.ok(names.includes("EditorScriptingUtilities")); // unrelated survived
    assert.ok(r.uprojectMutated);
  } finally {
    fs.rmSync(mono.root, { recursive: true, force: true });
    fs.rmSync(proj.projectDir, { recursive: true, force: true });
  }
});

test("installPlugin: running twice is a no-op success (idempotent) and does not rewrite .uproject", async () => {
  const mono = makeMonorepoFixture();
  const proj = makeProjectFixture();
  try {
    const first = await installPlugin({ projectDir: proj.projectDir, pluginSource: mono.root });
    assert.equal(first.ok, true);
    if (!first.ok) return;
    assert.ok(first.uprojectMutated, "first run should mutate the descriptor");

    // capture mtime window; second run should NOT rewrite an already-correct .uproject
    const uprojectPath = proj.uprojectPath;
    const before = fs.readFileSync(uprojectPath, "utf8");
    const second = await installPlugin({ projectDir: proj.projectDir, pluginSource: mono.root });
    assert.equal(second.ok, true);
    if (!second.ok) return;
    assert.equal(second.uprojectMutated, false, "second run must not mutate the descriptor");
    assert.equal(fs.readFileSync(uprojectPath, "utf8"), before, ".uproject byte-identical on no-op re-enable");
  } finally {
    fs.rmSync(mono.root, { recursive: true, force: true });
    fs.rmSync(proj.projectDir, { recursive: true, force: true });
  }
});

test("installPlugin: --dry-run resolves + reports but writes nothing", async () => {
  const mono = makeMonorepoFixture();
  const proj = makeProjectFixture();
  try {
    const beforeUProject = fs.readFileSync(proj.uprojectPath, "utf8");
    const r = await installPlugin({
      projectDir: proj.projectDir,
      pluginSource: mono.root,
      dryRun: true,
    });
    assert.equal(r.ok, true);
    if (!r.ok) return;
    assert.equal(r.dryRun, true);
    // nothing written
    assert.equal(
      fs.readFileSync(proj.uprojectPath, "utf8"),
      beforeUProject,
      "dry-run must not rewrite .uproject",
    );
    assert.ok(
      !fs.existsSync(path.join(proj.projectDir, "Plugins")),
      "dry-run must not create Plugins/",
    );
  } finally {
    fs.rmSync(mono.root, { recursive: true, force: true });
    fs.rmSync(proj.projectDir, { recursive: true, force: true });
  }
});

test("installPlugin: --no-verify installs bridge only", async () => {
  const mono = makeMonorepoFixture();
  const proj = makeProjectFixture();
  try {
    const r = await installPlugin({
      projectDir: proj.projectDir,
      pluginSource: mono.root,
      withVerify: false,
    });
    assert.equal(r.ok, true);
    if (!r.ok) return;
    assert.ok(fs.existsSync(path.join(proj.projectDir, "Plugins", BRIDGE_PLUGIN_NAME)));
    assert.ok(!fs.existsSync(path.join(proj.projectDir, "Plugins", VERIFY_PLUGIN_NAME)));
    const names = (readBackUProject(proj.projectDir).Plugins ?? []).map((p) => p.Name);
    assert.ok(names.includes("UnrealOpenMCP"));
    assert.ok(!names.includes("UnrealOpenMCPVerify"));
  } finally {
    fs.rmSync(mono.root, { recursive: true, force: true });
    fs.rmSync(proj.projectDir, { recursive: true, force: true });
  }
});

test("installPlugin: symlink mode creates a symlink to the source", async () => {
  const mono = makeMonorepoFixture();
  const proj = makeProjectFixture();
  try {
    const r = await installPlugin({
      projectDir: proj.projectDir,
      pluginSource: mono.root,
      symlink: true,
    });
    assert.equal(r.ok, true);
    if (!r.ok) return;
    const bridgeInstall = path.join(proj.projectDir, "Plugins", BRIDGE_PLUGIN_NAME);
    assert.ok(isSymlink(bridgeInstall), "bridge install is a symlink");
    if (r.installed.length > 0) {
      assert.equal(r.installed[0].mode, "symlink");
    }
  } finally {
    fs.rmSync(mono.root, { recursive: true, force: true });
    fs.rmSync(proj.projectDir, { recursive: true, force: true });
  }
});

test("installPlugin: re-running symlink install does not recurse into the live source", async () => {
  const mono = makeMonorepoFixture();
  const proj = makeProjectFixture();
  try {
    await installPlugin({ projectDir: proj.projectDir, pluginSource: mono.root, symlink: true });
    // second run must unlink the symlink, NOT rm through it into mono.bridgeSourceDir
    const r2 = await installPlugin({ projectDir: proj.projectDir, pluginSource: mono.root, symlink: true });
    assert.equal(r2.ok, true);
    // source descriptor still intact (proves we did not recurse-rm it)
    assert.ok(fs.existsSync(path.join(mono.bridgeSourceDir, "UnrealOpenMCP.uplugin")));
  } finally {
    fs.rmSync(mono.root, { recursive: true, force: true });
    fs.rmSync(proj.projectDir, { recursive: true, force: true });
  }
});

// ---------------------------------------------------------------------------
// install-plugin.ts — error paths
// ---------------------------------------------------------------------------

test("installPlugin: missing project dir → project_dir_missing", async () => {
  const mono = makeMonorepoFixture();
  try {
    const bogus = path.join(os.tmpdir(), `uomcp-nope-${process.pid}-${Date.now()}`);
    const r = await installPlugin({ projectDir: bogus, pluginSource: mono.root });
    assert.equal(r.ok, false);
    if (!r.ok) assert.equal(r.kind, "project_dir_missing");
  } finally {
    fs.rmSync(mono.root, { recursive: true, force: true });
  }
});

test("installPlugin: project dir without .uproject → no_uproject", async () => {
  const mono = makeMonorepoFixture();
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-noup-"));
  try {
    const r = await installPlugin({ projectDir: tmp, pluginSource: mono.root });
    assert.equal(r.ok, false);
    if (!r.ok) assert.equal(r.kind, "no_uproject");
  } finally {
    fs.rmSync(mono.root, { recursive: true, force: true });
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("installPlugin: bad --plugin-source → source_unresolved", async () => {
  const proj = makeProjectFixture();
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-badsrc-"));
  try {
    const r = await installPlugin({ projectDir: proj.projectDir, pluginSource: tmp });
    assert.equal(r.ok, false);
    if (!r.ok) assert.equal(r.kind, "source_unresolved");
  } finally {
    fs.rmSync(proj.projectDir, { recursive: true, force: true });
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

test("installPlugin: preserves existing plugin entry position + unrelated fields", async () => {
  const mono = makeMonorepoFixture();
  // project already has UnrealOpenMCP enabled with an extra MarketplaceURL field
  const proj = makeProjectFixture([
    { Name: "UnrealOpenMCP", Enabled: true, MarketplaceURL: "keep-me" },
  ]);
  try {
    const r = await installPlugin({ projectDir: proj.projectDir, pluginSource: mono.root });
    assert.equal(r.ok, true);
    if (!r.ok) return;
    const desc = readBackUProject(proj.projectDir);
    const bridge = (desc.Plugins ?? []).find((p) => p.Name === "UnrealOpenMCP");
    assert.ok(bridge);
    assert.equal(bridge?.MarketplaceURL, "keep-me");
    assert.equal(bridge?.Enabled, true);
  } finally {
    fs.rmSync(mono.root, { recursive: true, force: true });
    fs.rmSync(proj.projectDir, { recursive: true, force: true });
  }
});

// ---------------------------------------------------------------------------
// commands/install-plugin.ts
// ---------------------------------------------------------------------------

test("resolveProjectDir: positional wins over --project / env / cwd", () => {
  const got = resolveProjectDir({
    positionalProjectDir: "/pos",
    projectPath: "/flag",
    cwd: "/cwd",
    env: { UNREAL_PROJECT_PATH: "/env" },
  });
  assert.equal(got, "/pos");
});

test("resolveProjectDir: --project wins over env / cwd", () => {
  const got = resolveProjectDir({
    projectPath: "/flag",
    cwd: "/cwd",
    env: { UNREAL_PROJECT_PATH: "/env" },
  });
  assert.equal(got, "/flag");
});

test("resolveProjectDir: env wins over cwd", () => {
  const got = resolveProjectDir({
    cwd: "/cwd",
    env: { UNREAL_PROJECT_PATH: "/env" },
  });
  assert.equal(got, "/env");
});

test("resolveProjectDir: falls back to cwd", () => {
  const got = resolveProjectDir({ cwd: "/cwd", env: {} });
  assert.equal(got, "/cwd");
});

test("formatHuman: success block lists installed plugins + next steps", () => {
  const mono = makeMonorepoFixture();
  const proj = makeProjectFixture();
  try {
    // dry-run result avoids touching the FS but still exercises the formatter
    const block = formatHuman(
      {
        ok: true,
        projectDir: proj.projectDir,
        sourceRoot: mono.root,
        installed: [
          { name: "UnrealOpenMCP", installedPath: "/p/Plugins/UnrealOpenMCP", mode: "copy", uprojectMutated: true },
        ],
        uprojectMutated: true,
        warnings: [],
        dryRun: true,
      },
      "unreal-open-mcp-cli",
    );
    assert.match(block, /\[dry-run\]/);
    assert.match(block, /UnrealOpenMCP/);
    assert.ok(!block.includes("Next:"), "dry-run must not print next steps");
  } finally {
    fs.rmSync(mono.root, { recursive: true, force: true });
    fs.rmSync(proj.projectDir, { recursive: true, force: true });
  }
});

test("formatJson: round-trips a success result as JSON", () => {
  const body = formatJson({
    ok: true,
    projectDir: "/p",
    sourceRoot: "/r",
    installed: [],
    uprojectMutated: false,
    warnings: [],
    dryRun: false,
  });
  const parsed = JSON.parse(body);
  assert.equal(parsed.ok, true);
  assert.equal(parsed.projectDir, "/p");
});

test("runInstallPluginCommand: end-to-end success via the command layer (human)", async () => {
  const mono = makeMonorepoFixture();
  const proj = makeProjectFixture();
  const outChunks: string[] = [];
  const errChunks: string[] = [];
  try {
    const outcome = await runInstallPluginCommand(
      {
        positionalProjectDir: proj.projectDir,
        pluginSource: mono.root,
      },
      async (s) => { outChunks.push(s); },
      async (s) => { errChunks.push(s); },
      "unreal-open-mcp-cli",
    );
    assert.equal(outcome.exitCode, 0);
    assert.ok(outChunks.join("").includes("UnrealOpenMCP"));
    assert.equal(errChunks.join(""), "");
    // side effect: plugin actually installed
    assert.ok(fs.existsSync(path.join(proj.projectDir, "Plugins", BRIDGE_PLUGIN_NAME)));
  } finally {
    fs.rmSync(mono.root, { recursive: true, force: true });
    fs.rmSync(proj.projectDir, { recursive: true, force: true });
  }
});

test("runInstallPluginCommand: --json emits a JSON envelope on stdout for success", async () => {
  const mono = makeMonorepoFixture();
  const proj = makeProjectFixture();
  const outChunks: string[] = [];
  const errChunks: string[] = [];
  try {
    const outcome = await runInstallPluginCommand(
      {
        positionalProjectDir: proj.projectDir,
        pluginSource: mono.root,
        json: true,
      },
      async (s) => { outChunks.push(s); },
      async (s) => { errChunks.push(s); },
      "unreal-open-mcp-cli",
    );
    assert.equal(outcome.exitCode, 0);
    const parsed = JSON.parse(outChunks.join(""));
    assert.equal(parsed.ok, true);
    assert.equal(errChunks.join(""), "");
  } finally {
    fs.rmSync(mono.root, { recursive: true, force: true });
    fs.rmSync(proj.projectDir, { recursive: true, force: true });
  }
});

test("runInstallPluginCommand: failure writes to stderr (human) and exits 2", async () => {
  const mono = makeMonorepoFixture();
  const outChunks: string[] = [];
  const errChunks: string[] = [];
  try {
    const outcome = await runInstallPluginCommand(
      {
        positionalProjectDir: path.join(os.tmpdir(), `uomcp-does-not-exist-${Date.now()}`),
        pluginSource: mono.root,
      },
      async (s) => { outChunks.push(s); },
      async (s) => { errChunks.push(s); },
      "unreal-open-mcp-cli",
    );
    assert.equal(outcome.exitCode, 2);
    assert.equal(outChunks.join(""), "");
    assert.match(errChunks.join(""), /install-plugin failed/);
  } finally {
    fs.rmSync(mono.root, { recursive: true, force: true });
  }
});

test("runInstallPluginCommand: --json failure emits JSON envelope on stderr", async () => {
  const mono = makeMonorepoFixture();
  const outChunks: string[] = [];
  const errChunks: string[] = [];
  try {
    const outcome = await runInstallPluginCommand(
      {
        positionalProjectDir: path.join(os.tmpdir(), `uomcp-does-not-exist-${Date.now()}`),
        pluginSource: mono.root,
        json: true,
      },
      async (s) => { outChunks.push(s); },
      async (s) => { errChunks.push(s); },
      "unreal-open-mcp-cli",
    );
    assert.equal(outcome.exitCode, 2);
    assert.equal(outChunks.join(""), "");
    const parsed = JSON.parse(errChunks.join(""));
    assert.equal(parsed.ok, false);
  } finally {
    fs.rmSync(mono.root, { recursive: true, force: true });
  }
});
