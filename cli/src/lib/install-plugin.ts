// `install-plugin` — place (or symlink) the bridge + verify plugins under
// `<project>/Plugins/` and enable them in the `.uproject`, idempotently.
//
// Adapted from Unity Open MCP's local-path install philosophy and Unreal-MCP's
// `lib/install-plugin.ts` shape. Deltas (per P8.2 plan):
//   - copy | symlink — no junction (junction is Windows-only and the
//     `fs.symlinkSync(..., 'junction')` form is not portable across the macOS /
//     Linux dev floor; a plain symlink covers the dev-loop case on all three).
//   - No bundled-bridge stash dance — the Unreal Open MCP bridge is a C++ Editor
//     module compiled by UBT in-place, not a prebuilt sidecar under
//     `Binaries/ThirdParty`. We just exclude build artifacts.
//   - No GitHub release fetch — monorepo / local source first (ADR-001).
//
// Idempotency contract: running install twice is a no-op success. On a second
// run, a real directory is `rm`'d and re-copied (so source edits land), a
// symlink is `unlink`'d and re-linked, and the `.uproject` enable is upserted
// (a no-op when the entry is already `{ Name, Enabled: true }`).

import * as fs from "node:fs";
import * as path from "node:path";

import {
  BRIDGE_PLUGIN_NAME,
  VERIFY_PLUGIN_NAME,
  hasVerifySource,
  resolvePluginSource,
  type PluginSourceRoots,
} from "./plugin-source.js";
import {
  readUProject,
  upsertPluginEntry,
  writeUProject,
  type UProjectDescriptor,
} from "./uproject.js";

/** Install mode the caller requested (after platform / fallback resolution). */
export type InstallMode = "copy" | "symlink";

/** Per-plugin install outcome, returned in the top-level result. */
export interface InstalledPlugin {
  /** Plugin identity (matches the `.uplugin` and the install folder name). */
  name: string;
  /** Absolute install path (`<project>/Plugins/<name>`). */
  installedPath: string;
  /** Effective mode after platform / fallback resolution. */
  mode: InstallMode;
  /**
   * Whether the `.uproject` was mutated (an entry was inserted or changed).
   * `false` on a no-op re-enable.
   */
  uprojectMutated: boolean;
}

export interface InstallPluginOptions {
  /**
   * Absolute project root (the `.uproject` parent). Required — the installer
   * does not fall back to `process.cwd()` itself; the command layer resolves
   * that so the library stays pure-ish and testable with an explicit dir.
   */
  projectDir: string;
  /** `--plugin-source <dir>` override (a monorepo root). */
  pluginSource?: string;
  /**
   * `--symlink` — request a symlink install for the dev loop. Falls back to
   * copy on Windows when the process cannot create symlinks (Developer Mode /
   * admin), surfaced as a warning.
   */
  symlink?: boolean;
  /**
   * `--with-verify` / `--no-verify`. Default `true` (the bridge plugin's
   * `.uplugin` declares the verify plugin as an enabled dependency, so a
   * bridge-only install would leave the project in a half-resolved state).
   */
  withVerify?: boolean;
  /** `--dry-run` — resolve + report, but write nothing. */
  dryRun?: boolean;
}

export type InstallPluginErrorKind =
  | "project_dir_missing" // projectDir does not exist
  | "no_uproject" // no .uproject in projectDir
  | "uproject_unreadable" // read/parse failure (message carries detail)
  | "source_unresolved" // plugin source roots could not be resolved
  | "source_missing_descriptor" // bridge .uplugin absent at resolved source
  | "install_failed"; // filesystem op threw during copy/symlink/write

export interface InstallPluginError {
  kind: InstallPluginErrorKind;
  message: string;
}

export interface InstallPluginResultSuccess {
  ok: true;
  /** Absolute project root the install targeted. */
  projectDir: string;
  /** Resolved monorepo root the plugins were sourced from. */
  sourceRoot: string;
  /** Per-plugin install records (bridge first, then verify when installed). */
  installed: InstalledPlugin[];
  /** Whether ANY `.uproject` mutation happened (insert OR field change). */
  uprojectMutated: boolean;
  /** Non-fatal warnings (verify missing, symlink fallback, no-op re-install). */
  warnings: string[];
  /** True when nothing was written (--dry-run). */
  dryRun: boolean;
}

export type InstallPluginResult =
  | InstallPluginResultSuccess
  | ({ ok: false } & InstallPluginError);

/** Subtrees excluded from a copy install (UBT build artifacts). */
const COPY_EXCLUDE_DIRS = new Set(["Intermediate", "Binaries", ".vs", "DerivedDataCache"]);

/**
 * True when `p` is an existing symlink/junction (vs a real directory or file).
 * Centralised before destructive `rmSync`/`unlinkSync`: a symlink must be
 * `unlink`ed (never recursed into the live source), a real dir must be `rm`ed.
 */
export function isSymlink(p: string): boolean {
  try {
    return fs.lstatSync(p).isSymbolicLink();
  } catch {
    return false;
  }
}

/**
 * Decide whether `srcAbs` (absolute) should be copied into the target. Excludes
 * UBT build artifacts (`Intermediate/`, `Binaries/`) so a copy from a foreign
 * dev checkout never ships stale compiled modules (Unreal-MCP issue #60/#73
 * analog). Keeps `Source/`, `Resources/`, `Config/`, the descriptor, etc.
 */
function copyFilter(srcAbs: string, pluginSourceDir: string): boolean {
  const rel = path.relative(pluginSourceDir, srcAbs);
  if (rel === "" || rel === ".") return true; // plugin root
  const firstSeg = rel.split(path.sep)[0];
  if (COPY_EXCLUDE_DIRS.has(firstSeg)) return false;
  return true;
}

/**
 * Resolve which install mode to actually use given the request + platform.
 * Returns the effective mode and any warning (e.g. symlink fallback to copy).
 */
function resolveMode(
  requested: InstallMode | undefined,
  symlinkFlag: boolean | undefined,
): { mode: InstallMode; warning?: string } {
  if (symlinkFlag === true) {
    // Symlinks are first-class on macOS/Linux. On Windows they need Developer
    // Mode or admin; we ATTEMPT the symlink and let `fs.symlinkSync` throw,
    // which the caller converts to an `install_failed` error with a clear hint.
    // (A silent fallback to copy would hide a half-working dev loop.)
    return { mode: "symlink" };
  }
  return { mode: "copy" };
  void requested; // reserved for an explicit --mode flag later
}

/**
 * Remove a prior install path (symlink or real dir) so the operation is
 * idempotent and never nests a copy inside a stale symlink.
 */
function clearPriorInstall(installedPath: string): void {
  if (!fs.existsSync(installedPath) && !isSymlink(installedPath)) return;
  if (isSymlink(installedPath)) {
    fs.unlinkSync(installedPath);
  } else {
    fs.rmSync(installedPath, { recursive: true, force: true });
  }
}

export interface SingleInstallSpec {
  /** Plugin identity (folder name + `.uplugin`-implied Name). */
  name: string;
  /** Absolute source dir to install from. */
  sourceDir: string;
  /** Descriptor basename to sanity-check at the source root. */
  descriptor: string;
}

/**
 * Install a single plugin (bridge or verify) into `<project>/Plugins/<name>`
 * and enable it in the descriptor. Returns the install record + whether the
 * `.uproject` was mutated by THIS plugin's upsert (insert or field change).
 * Throws on filesystem failure (caller wraps).
 *
 * `descriptorMutationOf` is the caller's snapshot of the descriptor's serialized
 * form before any plugin ran; we compare before/after around THIS upsert to
 * detect whether it actually changed the descriptor (a no-op re-enable on an
 * already-`{ Name, Enabled: true }` entry leaves it byte-identical).
 */
function installOne(
  spec: SingleInstallSpec,
  projectDir: string,
  descriptor: UProjectDescriptor,
  mode: InstallMode,
  dryRun: boolean,
  beforeThisUpsertJson: string,
): InstalledPlugin {
  const pluginsDir = path.join(projectDir, "Plugins");
  const installedPath = path.join(pluginsDir, spec.name);

  if (!dryRun) {
    fs.mkdirSync(pluginsDir, { recursive: true });
    clearPriorInstall(installedPath);
    if (mode === "symlink") {
      fs.symlinkSync(spec.sourceDir, installedPath, "dir");
    } else {
      fs.cpSync(spec.sourceDir, installedPath, {
        recursive: true,
        filter: (src) => copyFilter(src, spec.sourceDir),
      });
    }
  }

  upsertPluginEntry(descriptor, spec.name);
  const afterThisUpsertJson = JSON.stringify(descriptor);
  return {
    name: spec.name,
    installedPath,
    mode,
    uprojectMutated: beforeThisUpsertJson !== afterThisUpsertJson,
  };
}

/**
 * Install the bridge (+ optionally verify) plugin(s) into a project and enable
 * them in the `.uproject`. Never throws — failures are structured `ok:false`
 * results so the CLI surfaces a clean exit code + message.
 */
export async function installPlugin(
  opts: InstallPluginOptions,
): Promise<InstallPluginResult> {
  const warnings: string[] = [];

  // --- project dir + .uproject -------------------------------------------
  const projectDir = path.resolve(opts.projectDir);
  if (!fs.existsSync(projectDir) || !fs.statSync(projectDir).isDirectory()) {
    return {
      ok: false,
      kind: "project_dir_missing",
      message: `Project directory does not exist or is not a directory: ${projectDir}`,
    };
  }

  const read = readUProject(projectDir);
  if (!read.ok) {
    if (read.kind === "not_found") {
      return { ok: false, kind: "no_uproject", message: read.message };
    }
    return { ok: false, kind: "uproject_unreadable", message: read.message };
  }

  // --- plugin source roots ------------------------------------------------
  const source = resolvePluginSource(opts.pluginSource);
  if (!source.ok) {
    return { ok: false, kind: "source_unresolved", message: source.message };
  }
  if (!fs.existsSync(path.join(source.bridgeSourceDir, "UnrealOpenMCP.uplugin"))) {
    return {
      ok: false,
      kind: "source_missing_descriptor",
      message: `Bridge plugin descriptor missing at ${source.bridgeSourceDir}/UnrealOpenMCP.uplugin.`,
    };
  }

  // --- decide what to install --------------------------------------------
  const specs: SingleInstallSpec[] = [
    {
      name: BRIDGE_PLUGIN_NAME,
      sourceDir: source.bridgeSourceDir,
      descriptor: "UnrealOpenMCP.uplugin",
    },
  ];
  const wantVerify = opts.withVerify !== false; // default true
  if (wantVerify) {
    if (hasVerifySource(source)) {
      specs.push({
        name: VERIFY_PLUGIN_NAME,
        sourceDir: source.verifySourceDir,
        descriptor: "UnrealOpenMCPVerify.uplugin",
      });
    } else {
      warnings.push(
        `--with-verify requested but no verify plugin source found at ${source.verifySourceDir}; installing bridge only.`,
      );
    }
  }

  // Snapshot the descriptor's serialized form and roll it forward across each
  // upsert. A no-op re-enable (every entry already `{ Name, Enabled: true }`)
  // leaves the descriptor byte-identical end-to-end, so we skip the file write
  // AND each per-plugin record reports `uprojectMutated:false`.
  let rollingJson = JSON.stringify(read.descriptor);
  const beforeAnyUpsertJson = rollingJson;
  const { mode, warning: modeWarning } = resolveMode(undefined, opts.symlink);
  if (modeWarning) warnings.push(modeWarning);

  const installed: InstalledPlugin[] = [];
  for (const spec of specs) {
    const record = installOne(spec, projectDir, read.descriptor, mode, !!opts.dryRun, rollingJson);
    installed.push(record);
    rollingJson = JSON.stringify(read.descriptor);
  }

  const uprojectMutated = beforeAnyUpsertJson !== rollingJson;

  if (!opts.dryRun && uprojectMutated) {
    try {
      writeUProject(read.descriptor, read.uprojectPath);
    } catch (e) {
      return {
        ok: false,
        kind: "install_failed",
        message: `Installed plugin files but failed to write ${read.uprojectPath}: ${(e as Error).message}`,
      };
    }
  }

  if (!opts.dryRun && !uprojectMutated) {
    warnings.push("All requested plugins were already enabled in the .uproject; descriptor not rewritten.");
  }

  return {
    ok: true,
    projectDir,
    sourceRoot: source.root,
    installed,
    uprojectMutated,
    warnings,
    dryRun: !!opts.dryRun,
  };
}

// Re-export so the command layer can build help text / branching from one import.
export { hasVerifySource, type PluginSourceRoots };
