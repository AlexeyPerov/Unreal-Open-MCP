// Tests for `open.ts` (lib/open.ts + commands/open.ts). Temp-dir based — no
// real engine, no spawn, no network. The spawn + fs-exists surfaces are
// injected so the engine resolution + launch-arg + env construction are fully
// deterministic and never touch the host.
//
// Built + run via the package test config (see package.json `test`).

import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import * as os from "node:os";

import {
  openProject,
  resolveEngine,
  editorBinaryRelPath,
  buildOpenEnv,
  buildEditorArgs,
  type OpenProjectOptions,
} from "./open.js";
import {
  formatHuman,
  formatJson,
  resolveProjectDir,
  runOpenCommand,
} from "../commands/open.js";
import { PROJECT_PATH_ENV_VAR, PORT_ENV_VAR } from "../constants.js";

// ---------------------------------------------------------------------------
// fixtures
// ---------------------------------------------------------------------------

/**
 * A temp project dir with a `.uproject`. `EngineAssociation` defaults to "5.7"
 * but is overridable so the association-resolution path can be exercised.
 */
function makeProjectFixture(opts: { engineAssociation?: string } = {}): string {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-open-project-"));
  const uproject = {
    EngineAssociation: opts.engineAssociation ?? "5.7",
    Modules: [],
  };
  // The .uproject name does not matter — findUProjectFile picks the first.
  fs.writeFileSync(
    path.join(dir, "MyGame.uproject"),
    JSON.stringify(uproject, null, 2),
  );
  return dir;
}

/** A fake engine install root containing the per-OS editor binary. */
function makeEngineFixture(plat: NodeJS.Platform): string {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-engine-"));
  const rel = editorBinaryRelPath(plat);
  const editorPath = path.join(root, rel);
  fs.mkdirSync(path.dirname(editorPath), { recursive: true });
  fs.writeFileSync(editorPath, "#!/bin/sh\n# fake editor\n");
  return root;
}

// ---------------------------------------------------------------------------
// editorBinaryRelPath
// ---------------------------------------------------------------------------

test("editorBinaryRelPath: win32 -> Engine/Binaries/Win64/UnrealEditor.exe", () => {
  const rel = editorBinaryRelPath("win32");
  assert.equal(rel, path.join("Engine", "Binaries", "Win64", "UnrealEditor.exe"));
});

test("editorBinaryRelPath: darwin -> Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor", () => {
  const rel = editorBinaryRelPath("darwin");
  assert.ok(rel.includes(path.join("Engine", "Binaries", "Mac")));
  assert.ok(rel.endsWith(path.join("UnrealEditor.app", "Contents", "MacOS", "UnrealEditor")));
});

test("editorBinaryRelPath: linux -> Engine/Binaries/Linux/UnrealEditor", () => {
  const rel = editorBinaryRelPath("linux");
  assert.equal(rel, path.join("Engine", "Binaries", "Linux", "UnrealEditor"));
});

// ---------------------------------------------------------------------------
// buildOpenEnv / buildEditorArgs
// ---------------------------------------------------------------------------

test("buildOpenEnv: always carries UNREAL_PROJECT_PATH", () => {
  const env = buildOpenEnv({ projectDir: "/p/MyGame" });
  assert.equal(env[PROJECT_PATH_ENV_VAR], "/p/MyGame");
  assert.equal(env[PORT_ENV_VAR], undefined);
});

test("buildOpenEnv: bridge port override is included only when set", () => {
  const env = buildOpenEnv({ projectDir: "/p/MyGame", bridgePort: 23456 });
  assert.equal(env[PORT_ENV_VAR], "23456");
});

test("buildOpenEnv: never sets cloud / OAuth / transport vars (ADR-001 skip)", () => {
  const env = buildOpenEnv({ projectDir: "/p/MyGame", bridgePort: 23456 });
  // None of the Unreal-MCP cloud vars leak through.
  assert.equal(env["UNREAL_MCP_HOST"], undefined);
  assert.equal(env["UNREAL_MCP_TOKEN"], undefined);
  assert.equal(env["UNREAL_MCP_AUTH_OPTION"], undefined);
  assert.equal(env["UNREAL_MCP_TRANSPORT"], undefined);
  assert.equal(env["UNREAL_MCP_CONNECTION_MODE"], undefined);
});

test("buildEditorArgs: uproject path first; port arg appended only when set", () => {
  assert.deepEqual(buildEditorArgs("/p/MyGame/MyGame.uproject"), [
    "/p/MyGame/MyGame.uproject",
  ]);
  assert.deepEqual(buildEditorArgs("/p/MyGame/MyGame.uproject", 23456), [
    "/p/MyGame/MyGame.uproject",
    "-UNREAL_OPEN_MCP_BRIDGE_PORT=23456",
  ]);
});

// ---------------------------------------------------------------------------
// resolveEngine
// ---------------------------------------------------------------------------

test("resolveEngine: --engine-root override wins and is verified", () => {
  const engineRoot = makeEngineFixture("darwin");
  const r = resolveEngine({
    engineAssociation: "5.7",
    engineRootOverride: engineRoot,
    os: "darwin",
    existsImpl: (p) => fs.existsSync(p),
  });
  assert.equal(r.kind, "resolved");
  if (r.kind === "resolved") {
    assert.equal(r.source, "override");
    assert.equal(r.engineRoot, engineRoot);
  }
});

test("resolveEngine: --engine-root without an editor binary => unresolved + hint", () => {
  const empty = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-empty-"));
  try {
    const r = resolveEngine({
      engineAssociation: "5.7",
      engineRootOverride: empty,
      os: "darwin",
      existsImpl: (p) => fs.existsSync(p),
    });
    assert.equal(r.kind, "unresolved");
    if (r.kind === "unresolved") {
      assert.match(r.message, /--engine-root/);
    }
  } finally {
    fs.rmSync(empty, { recursive: true, force: true });
  }
});

test("resolveEngine: UE_ROOT env resolves when no override", () => {
  const engineRoot = makeEngineFixture("darwin");
  try {
    const r = resolveEngine({
      engineAssociation: "5.7",
      os: "darwin",
      env: { UE_ROOT: engineRoot },
      existsImpl: (p) => fs.existsSync(p),
    });
    assert.equal(r.kind, "resolved");
    if (r.kind === "resolved") {
      assert.equal(r.source, "env");
      assert.equal(r.engineRoot, engineRoot);
    }
  } finally {
    fs.rmSync(engineRoot, { recursive: true, force: true });
  }
});

test("resolveEngine: GUID association is never matched by the common-path scan", () => {
  // A GUID names a source build; the scan cannot resolve it. UE_ROOT unset,
  // no override => unresolved.
  const r = resolveEngine({
    engineAssociation: "{0A1B2C3D-0000-0000-0000-000000000000}",
    os: "darwin",
    env: {},
    existsImpl: () => false, // nothing on disk
  });
  assert.equal(r.kind, "unresolved");
  if (r.kind === "unresolved") {
    assert.match(r.message, /--engine-root|UE_ROOT/);
  }
});

test("resolveEngine: nothing resolves => unresolved with both hints", () => {
  const r = resolveEngine({
    engineAssociation: "5.7",
    os: "darwin",
    env: {},
    existsImpl: () => false,
  });
  assert.equal(r.kind, "unresolved");
  if (r.kind === "unresolved") {
    assert.match(r.message, /--engine-root/);
    assert.match(r.message, /UE_ROOT/);
  }
});

// ---------------------------------------------------------------------------
// openProject (spawn + env + arg construction)
// ---------------------------------------------------------------------------

test("openProject: happy path — resolves engine, spawns with project path + env", async () => {
  const projectDir = makeProjectFixture();
  const engineRoot = makeEngineFixture("darwin");
  let spawnedEditor: string | undefined;
  let spawnedArgs: string[] | undefined;
  let spawnedEnv: NodeJS.ProcessEnv | undefined;
  try {
    const opts: OpenProjectOptions = {
      projectDir,
      engineRoot: engineRoot,
      platform: "darwin",
      env: { PATH: "/usr/bin" },
      existsImpl: (p) => fs.existsSync(p),
      spawnImpl: (editorPath, args, env) => {
        spawnedEditor = editorPath;
        spawnedArgs = args;
        spawnedEnv = env;
        return { pid: 4242 };
      },
    };
    const result = await openProject(opts);
    assert.equal(result.kind, "success");
    if (result.kind !== "success") return;
    assert.equal(result.success, true);
    assert.equal(result.editorPid, 4242);
    assert.equal(result.engineSource, "override");
    assert.equal(result.projectDir, projectDir);
    // The .uproject path is the first launch arg.
    assert.equal(spawnedArgs?.[0], path.join(projectDir, "MyGame.uproject"));
    assert.equal(spawnedEditor, result.editorPath);
    // The env carries PROJECT_PATH (absolute project dir) and inherits PATH.
    assert.equal(spawnedEnv?.[PROJECT_PATH_ENV_VAR], projectDir);
    assert.equal(spawnedEnv?.PATH, "/usr/bin");
  } finally {
    fs.rmSync(projectDir, { recursive: true, force: true });
    fs.rmSync(engineRoot, { recursive: true, force: true });
  }
});

test("openProject: bridge port override appends the launch arg + env var", async () => {
  const projectDir = makeProjectFixture();
  const engineRoot = makeEngineFixture("darwin");
  let spawnedArgs: string[] | undefined;
  let spawnedEnv: NodeJS.ProcessEnv | undefined;
  try {
    const result = await openProject({
      projectDir,
      engineRoot: engineRoot,
      bridgePort: 23456,
      platform: "darwin",
      env: {},
      existsImpl: (p) => fs.existsSync(p),
      spawnImpl: (_e, args, env) => {
        spawnedArgs = args;
        spawnedEnv = env;
        return { pid: 99 };
      },
    });
    assert.equal(result.kind, "success");
    assert.ok(spawnedArgs?.includes("-UNREAL_OPEN_MCP_BRIDGE_PORT=23456"));
    assert.equal(spawnedEnv?.[PORT_ENV_VAR], "23456");
  } finally {
    fs.rmSync(projectDir, { recursive: true, force: true });
    fs.rmSync(engineRoot, { recursive: true, force: true });
  }
});

test("openProject: missing project dir => failure with a clear message", async () => {
  const result = await openProject({
    projectDir: "/nonexistent/uomcp/project",
    engineRoot: "/nonexistent/engine",
    platform: "darwin",
    env: {},
    existsImpl: () => false,
    spawnImpl: () => ({ pid: 1 }),
  });
  assert.equal(result.kind, "failure");
  if (result.kind === "failure") {
    assert.match(result.errorMessage, /does not exist/i);
  }
});

test("openProject: project dir with no .uproject => failure", async () => {
  const empty = fs.mkdtempSync(path.join(os.tmpdir(), "uomcp-nouproject-"));
  try {
    const result = await openProject({
      projectDir: empty,
      engineRoot: makeEngineFixture("darwin"),
      platform: "darwin",
      env: {},
      existsImpl: (p) => fs.existsSync(p),
      spawnImpl: () => ({ pid: 1 }),
    });
    assert.equal(result.kind, "failure");
    if (result.kind === "failure") {
      assert.match(result.errorMessage, /\.uproject/i);
    }
  } finally {
    fs.rmSync(empty, { recursive: true, force: true });
  }
});

test("openProject: unresolved engine => failure with --engine-root hint", async () => {
  const projectDir = makeProjectFixture();
  try {
    const result = await openProject({
      projectDir,
      // No engineRoot, no UE_ROOT, association "5.7" not on disk.
      platform: "darwin",
      env: {},
      existsImpl: () => false,
      spawnImpl: () => ({ pid: 1 }),
    });
    assert.equal(result.kind, "failure");
    if (result.kind === "failure") {
      assert.match(result.errorMessage, /--engine-root|UE_ROOT/);
    }
  } finally {
    fs.rmSync(projectDir, { recursive: true, force: true });
  }
});

test("openProject: spawn returns no pid => success but undefined pid (best-effort)", async () => {
  const projectDir = makeProjectFixture();
  const engineRoot = makeEngineFixture("darwin");
  try {
    const result = await openProject({
      projectDir,
      engineRoot,
      platform: "darwin",
      env: {},
      existsImpl: (p) => fs.existsSync(p),
      spawnImpl: () => ({ pid: undefined }),
    });
    assert.equal(result.kind, "success");
    if (result.kind === "success") {
      assert.equal(result.editorPid, undefined);
    }
  } finally {
    fs.rmSync(projectDir, { recursive: true, force: true });
    fs.rmSync(engineRoot, { recursive: true, force: true });
  }
});

// ---------------------------------------------------------------------------
// command layer
// ---------------------------------------------------------------------------

test("resolveProjectDir: positional > --project > env > cwd", () => {
  const cwd = "/cwd";
  assert.equal(
    resolveProjectDir({
      positionalProjectDir: "/pos",
      projectPath: "/flag",
      env: { [PROJECT_PATH_ENV_VAR]: "/env" },
      cwd,
    }),
    "/pos",
  );
  assert.equal(
    resolveProjectDir({
      projectPath: "/flag",
      env: { [PROJECT_PATH_ENV_VAR]: "/env" },
      cwd,
    }),
    "/flag",
  );
  assert.equal(
    resolveProjectDir({
      env: { [PROJECT_PATH_ENV_VAR]: "/env" },
      cwd,
    }),
    "/env",
  );
  assert.equal(resolveProjectDir({ env: {}, cwd }), "/cwd");
});

test("formatHuman: success lists editor / engine / project + wait-for-ready hint", () => {
  const result = {
    kind: "success" as const,
    success: true as const,
    projectDir: "/p/MyGame",
    uprojectPath: "/p/MyGame/MyGame.uproject",
    editorPath: "/eng/UnrealEditor",
    engineRoot: "/eng",
    editorPid: 4242,
    engineSource: "override" as const,
    envVars: { [PROJECT_PATH_ENV_VAR]: "/p/MyGame" },
    warnings: [],
  };
  const txt = formatHuman(result, "unreal-open-mcp-cli");
  assert.match(txt, /PID: 4242/);
  assert.match(txt, /\/eng\/UnrealEditor/);
  assert.match(txt, /wait-for-ready/);
});

test("formatHuman: failure prints the error", () => {
  const result = {
    kind: "failure" as const,
    success: false as const,
    projectDir: undefined,
    warnings: [],
    errorMessage: "boom",
  };
  const txt = formatHuman(result, "unreal-open-mcp-cli");
  assert.match(txt, /open failed: boom/);
});

test("formatJson: success envelope round-trips with kind discriminant", () => {
  const result = {
    kind: "success" as const,
    success: true as const,
    projectDir: "/p/MyGame",
    uprojectPath: "/p/MyGame/MyGame.uproject",
    editorPath: "/eng/UnrealEditor",
    engineRoot: "/eng",
    editorPid: 4242,
    engineSource: "override" as const,
    envVars: {},
    warnings: [],
  };
  const parsed = JSON.parse(formatJson(result));
  assert.equal(parsed.kind, "success");
  assert.equal(parsed.success, true);
});

test("runOpenCommand: failure path (no engine) writes to stderr, exit 2", async () => {
  const projectDir = makeProjectFixture();
  const out: string[] = [];
  const errOut: string[] = [];
  try {
    const outcome = await runOpenCommand(
      {
        projectPath: projectDir,
        // No engineRoot, no UE_ROOT, association not on disk => unresolved.
        env: {},
        cwd: projectDir,
      },
      async (s) => {
        out.push(s);
      },
      async (s) => {
        errOut.push(s);
      },
      "unreal-open-mcp-cli",
    );
    assert.equal(outcome.exitCode, 2);
    assert.equal(out.length, 0);
    assert.ok(errOut.length > 0);
    assert.match(errOut.join(""), /open failed/);
  } finally {
    fs.rmSync(projectDir, { recursive: true, force: true });
  }
});

test("runOpenCommand: --json failure emits JSON envelope on stderr, exit 2", async () => {
  const projectDir = makeProjectFixture();
  const errOut: string[] = [];
  try {
    const outcome = await runOpenCommand(
      {
        projectPath: projectDir,
        json: true,
        env: {},
        cwd: projectDir,
      },
      async () => {},
      async (s) => {
        errOut.push(s);
      },
      "unreal-open-mcp-cli",
    );
    assert.equal(outcome.exitCode, 2);
    const parsed = JSON.parse(errOut.join(""));
    assert.equal(parsed.kind, "failure");
    assert.equal(parsed.success, false);
  } finally {
    fs.rmSync(projectDir, { recursive: true, force: true });
  }
});
