// `open` — launch the Unreal Editor for a project, wiring the Open MCP bridge
// env vars. Library-safe: never prints, never exits, never throws past the
// boundary.
//
// Adapted from Unreal-MCP's cli/src/lib/open.ts (read-only behavior reference)
// for engine resolution + launch flags, with these intentional deltas:
//   - STRIP cloud / OAuth env wiring (UNREAL_MCP_HOST / _TOKEN / _AUTH_OPTION /
//     _TRANSPORT / _CONNECTION_MODE / _START_SERVER) — ADR-001 cloud skip. The
//     only env the Open MCP bridge needs is `UNREAL_PROJECT_PATH` and the
//     optional `-UNREAL_OPEN_MCP_BRIDGE_PORT=` launch arg (P1.4 contract).
//   - STRIP the launcher-manifest + Windows-registry + common-location scan
//     discovery chain. The plan calls engine discovery "best-effort" with
//     `--engine-root` as the source-build escape hatch. This module resolves
//     the engine through a small, dependency-free precedence:
//       1. `--engine-root <dir>` (explicit; must contain Engine/Binaries/...)
//       2. `UE_ROOT` env var
//       3. `EngineAssociation` from the `.uproject` matched against per-OS
//          common install paths (Epic Launcher defaults).
//     When none resolve, the caller surfaces a clear `--engine-root` hint.
//   - STRIP the pre-launch UBT build step (`--no-build` is accepted for forward
//     compat but the MVP does not invoke UBT; a future plan can add it). The
//     bridge is a C++ Editor module compiled by UBT — the editor itself
//     triggers the build on launch when needed, so the CLI does not have to.

import * as fs from "node:fs";
import * as path from "node:path";
import { spawn, type ChildProcess } from "node:child_process";

import { readUProject } from "./uproject.js";
import { PROJECT_PATH_ENV_VAR, PORT_ENV_VAR } from "../constants.js";

/** Exit-code-adjacent result kind (the command maps failure -> exit 2). */
export type OpenOutcomeKind = "success" | "failure";

export interface OpenProjectOptions {
  /** Absolute project root (the `.uproject` parent). Required. */
  projectDir: string;
  /** `--engine-root <dir>` override (a source-build escape hatch). */
  engineRoot?: string;
  /**
   * `--no-build` — accepted for forward compat; the MVP does not invoke UBT
   * before launch (the editor triggers the build itself). Surfaced in the
   * result so the command can echo it.
   */
  noBuild?: boolean;
  /**
   * Bridge port override (`--port`). When set, propagated to the editor via the
   * `-UNREAL_OPEN_MCP_BRIDGE_PORT=<n>` launch arg so the bridge binds the port
   * the MCP server / wait-for-ready will poll. Optional; when absent the bridge
   * uses its deterministic per-project port.
   */
  bridgePort?: number;
  /**
   * Injectable spawn (default: child_process.spawn detached). Tests pass a
   * stub. The stub receives (editorPath, args, env) and must return an object
   * with a `pid` field (or undefined).
   */
  spawnImpl?: (
    editorPath: string,
    args: string[],
    env: NodeJS.ProcessEnv,
  ) => { pid?: number };
  /**
   * Injectable platform (default: process.platform). Tests pass a fixed value
   * so the editor-binary path + install-path scan are deterministic.
   */
  platform?: NodeJS.Platform;
  /**
   * Injectable env (default: process.env). Used to read `UE_ROOT` and as the
   * base for the child env.
   */
  env?: NodeJS.ProcessEnv;
  /** Injectable fs-exists (default: fs.existsSync). */
  existsImpl?: (p: string) => boolean;
}

export interface OpenProjectResultSuccess {
  kind: "success";
  success: true;
  /** Absolute project root (resolved from the `.uproject`'s dir). */
  projectDir: string;
  /** Absolute `.uproject` path passed as the editor's first positional arg. */
  uprojectPath: string;
  /** Absolute editor binary path that was spawned. */
  editorPath: string;
  /** Absolute engine root the editor binary lives under. */
  engineRoot: string;
  /** Spawned child PID (undefined when the spawn did not return one). */
  editorPid: number | undefined;
  /** How the engine was resolved (for status / logging). */
  engineSource: EngineSource;
  /** Env vars injected onto the editor process (always carries PROJECT_PATH). */
  envVars: Record<string, string>;
  /** Non-fatal warnings. */
  warnings: string[];
}

export interface OpenProjectResultFailure {
  kind: "failure";
  success: false;
  projectDir: string | undefined;
  warnings: string[];
  errorMessage: string;
}

export type OpenProjectResult = OpenProjectResultSuccess | OpenProjectResultFailure;

/** How the engine root was resolved (echoed in the result + status output). */
export type EngineSource =
  | "override" // --engine-root
  | "env" // UE_ROOT
  | "association" // .uproject EngineAssociation -> common install path
  | "detached"; // spawn returned but no pid (best-effort)

// ---------------------------------------------------------------------------
// Engine discovery (best-effort, dependency-free)
// ---------------------------------------------------------------------------

/**
 * Per-OS candidate engine install roots probed for an `EngineAssociation`-based
 * resolution. These are the Epic Launcher's default install locations; source
 * builds should use `--engine-root`. Mirrors the common-location scan concept
 * from Unreal-MCP's engine discovery, narrowed to the few paths that need no
 * registry / manifest parsing.
 *
 * The `assoc` (e.g. `"5.7"`) is substituted into `${assoc}` in each path.
 */
function commonEngineRoots(os: NodeJS.Platform, assoc: string): string[] {
  const home = process.env.HOME ?? process.env.USERPROFILE ?? "";
  switch (os) {
    case "win32":
      return [
        `C:\\Program Files\\Epic Games\\UE_${assoc}`,
        `C:\\Program Files (x86)\\Epic Games\\UE_${assoc}`,
      ];
    case "darwin":
      return [
        `/Users/${process.env.USER ?? "Shared"}/Library/Epic/UE_${assoc}`,
        path.join(home, "Library", "Epic", `UE_${assoc}`),
        `/Applications/UE_${assoc}`,
      ];
    default:
      // Linux: no canonical Epic install location; users typically set UE_ROOT.
      return [
        path.join(home, "UnrealEngine", `UE_${assoc}`),
        `/opt/UE_${assoc}`,
      ];
  }
}

/**
 * Relative path from an engine root to the editor binary, per OS. Matches
 * `Engine/Binaries/<plat>/UnrealEditor[.exe]`.
 */
export function editorBinaryRelPath(os: NodeJS.Platform): string {
  switch (os) {
    case "win32":
      return path.join("Engine", "Binaries", "Win64", "UnrealEditor.exe");
    case "darwin":
      return path.join("Engine", "Binaries", "Mac", "UnrealEditor.app", "Contents", "MacOS", "UnrealEditor");
    default:
      return path.join("Engine", "Binaries", "Linux", "UnrealEditor");
  }
}

export type EngineResolutionKind = "resolved" | "unresolved";

export interface EngineResolutionResolved {
  kind: "resolved";
  engineRoot: string;
  editorPath: string;
  source: EngineSource;
}

export interface EngineResolutionUnresolved {
  kind: "unresolved";
  source: null;
  message: string;
}

export type EngineResolution = EngineResolutionResolved | EngineResolutionUnresolved;

/**
 * Resolve the engine root + editor binary for a project, best-effort. Never
 * throws — failures are structured `kind:"unresolved"` so the caller surfaces a
 * clean CLI error + `--engine-root` hint.
 *
 * Precedence:
 *   1. `--engine-root <dir>` (must contain the editor binary at the per-OS
 *      relative path).
 *   2. `UE_ROOT` env var (same shape check).
 *   3. `.uproject` `EngineAssociation` matched against per-OS common install
 *      paths.
 *
 * An `EngineAssociation` that is a GUID (`{...}`) names a source build the
 * common-path scan cannot resolve — the caller must use `--engine-root`.
 */
export function resolveEngine(opts: {
  engineAssociation: string;
  engineRootOverride?: string;
  os: NodeJS.Platform;
  env?: NodeJS.ProcessEnv;
  existsImpl?: (p: string) => boolean;
}): EngineResolution {
  const os = opts.os;
  const exists = opts.existsImpl ?? ((p: string) => fs.existsSync(p));
  const env = opts.env ?? process.env;
  const rel = editorBinaryRelPath(os);

  // 1. explicit --engine-root
  if (opts.engineRootOverride && opts.engineRootOverride.trim().length > 0) {
    const root = path.resolve(opts.engineRootOverride);
    const editorPath = path.join(root, rel);
    if (exists(editorPath)) {
      return { kind: "resolved", engineRoot: root, editorPath, source: "override" };
    }
    return {
      kind: "unresolved",
      source: null,
      message:
        `--engine-root '${opts.engineRootOverride}' does not contain an editor ` +
        `binary at '${rel}'. Pass the engine install root (the directory that ` +
        `contains 'Engine/Binaries/...').`,
    };
  }

  // 2. UE_ROOT env
  const ueRoot = env["UE_ROOT"];
  if (ueRoot && ueRoot.trim().length > 0) {
    const root = path.resolve(ueRoot);
    const editorPath = path.join(root, rel);
    if (exists(editorPath)) {
      return { kind: "resolved", engineRoot: root, editorPath, source: "env" };
    }
  }

  // 3. EngineAssociation -> common install paths (skip GUIDs).
  const assoc = (opts.engineAssociation ?? "").trim();
  if (assoc && !assoc.startsWith("{")) {
    for (const candidate of commonEngineRoots(os, assoc)) {
      const editorPath = path.join(candidate, rel);
      if (exists(editorPath)) {
        return {
          kind: "resolved",
          engineRoot: candidate,
          editorPath,
          source: "association",
        };
      }
    }
  }

  return {
    kind: "unresolved",
    source: null,
    message:
      `Could not resolve the Unreal engine for association '${assoc || "(none)"}'. ` +
      `Pass --engine-root <dir> or set $UE_ROOT to the engine install root ` +
      `(the directory containing 'Engine/Binaries/...').`,
  };
}

/**
 * Build the Open MCP env-var map propagated to the editor process. Always
 * carries `UNREAL_PROJECT_PATH`; carries `UNREAL_OPEN_MCP_BRIDGE_PORT` only when
 * a port override is in use (the bridge otherwise uses its deterministic
 * per-project port). Pure.
 *
 * NOTE: this never sets cloud / OAuth / connection-mode vars (ADR-001 cloud
 * skip). The Open MCP bridge has no such env surface.
 */
export function buildOpenEnv(opts: {
  projectDir: string;
  bridgePort?: number;
}): Record<string, string> {
  const env: Record<string, string> = {
    [PROJECT_PATH_ENV_VAR]: opts.projectDir,
  };
  if (opts.bridgePort !== undefined) {
    env[PORT_ENV_VAR] = String(opts.bridgePort);
  }
  return env;
}

/**
 * Build the editor launch args. Always passes the `.uproject` path as the first
 * positional; appends `-UNREAL_OPEN_MCP_BRIDGE_PORT=<n>` when a port override
 * is set (the bridge reads this P1.4 launch arg).
 *
 * Exported for tests.
 */
export function buildEditorArgs(uprojectPath: string, bridgePort?: number): string[] {
  const args = [uprojectPath];
  if (bridgePort !== undefined) {
    args.push(`-UNREAL_OPEN_MCP_BRIDGE_PORT=${bridgePort}`);
  }
  return args;
}

/**
 * Launch the Unreal Editor for a project. Never throws past the boundary —
 * failures are structured `kind:"failure"` results so the CLI surfaces a clean
 * exit code + message.
 */
export async function openProject(opts: OpenProjectOptions): Promise<OpenProjectResult> {
  const warnings: string[] = [];
  const os = opts.platform ?? (process.platform as NodeJS.Platform);
  const env = opts.env ?? process.env;

  let projectDir: string | undefined;
  try {
    const inputPath = path.resolve(opts.projectDir);
    projectDir = inputPath;

    if (!fs.existsSync(inputPath)) {
      throw new Error(`Project path does not exist: ${inputPath}`);
    }
    const uproject = readUProject(inputPath);
    if (!uproject.ok) {
      // readUProject's `not_found` is the common case (user passed a dir with
      // no .uproject); surface its message verbatim.
      throw new Error(uproject.message);
    }
    projectDir = uproject.projectDir;

    const resolution = resolveEngine({
      engineAssociation: uproject.descriptor.EngineAssociation ?? "",
      engineRootOverride: opts.engineRoot,
      os,
      env,
      existsImpl: opts.existsImpl,
    });
    if (resolution.kind === "unresolved") {
      throw new Error(resolution.message);
    }

    const envVars = buildOpenEnv({ projectDir, bridgePort: opts.bridgePort });
    const args = buildEditorArgs(uproject.uprojectPath, opts.bridgePort);
    const childEnv: NodeJS.ProcessEnv = { ...env, ...envVars };

    const spawnFn =
      opts.spawnImpl ??
      ((editorPath: string, spawnArgs: string[], spawnEnv: NodeJS.ProcessEnv) =>
        spawnDetached(editorPath, spawnArgs, spawnEnv));
    const child = spawnFn(resolution.editorPath, args, childEnv);

    return {
      kind: "success",
      success: true,
      projectDir,
      uprojectPath: uproject.uprojectPath,
      editorPath: resolution.editorPath,
      engineRoot: resolution.engineRoot,
      editorPid: child.pid,
      engineSource: resolution.source,
      envVars,
      warnings,
    };
  } catch (err: unknown) {
    const error = err instanceof Error ? err : new Error(String(err));
    return {
      kind: "failure",
      success: false,
      projectDir,
      warnings,
      errorMessage: error.message,
    };
  }
}

/**
 * Detached spawn — fire-and-forget. The editor runs out-of-band; the CLI
 * returns immediately and `wait-for-ready` polls until the bridge binds. The
 * child's stdio is ignored so the editor's console output never tangles with
 * the CLI's.
 *
 * A detached spawn can fail asynchronously (EACCES, ENOENT) AFTER the call
 * returns; we install an 'error' listener to avoid an unhandled 'error' event
 * crashing the CLI, then `unref` so the child does not keep the CLI process
 * alive. The async failure surfaces as a missing pid + a `wait-for-ready`
 * timeout — the same UX Unity / Unreal-MCP ship.
 */
function spawnDetached(
  editorPath: string,
  args: string[],
  env: NodeJS.ProcessEnv,
): { pid?: number } {
  const child: ChildProcess = spawn(editorPath, args, {
    detached: true,
    stdio: "ignore",
    env,
  });
  child.on("error", () => {
    // Swallowed — the editor simply did not launch. `wait-for-ready` will time
    // out and report the bridge unreachable.
  });
  try {
    child.unref();
  } catch {
    // unref can throw on an already-errored child; safe to ignore.
  }
  return { pid: child.pid };
}
