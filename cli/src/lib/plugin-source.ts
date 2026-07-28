// Resolve the bridge / verify plugin source directories for `install-plugin`.
//
// Adapted from Unity Open MCP's local-path, non-cloud install philosophy and
// Unreal-MCP's `lib/plugin-source.ts` shape, with the GitHub-release fetch
// DELIBERATELY SKIPPED (P8.2 fidelity tag: skip — monorepo/local source first,
// per ADR-001 cloud-auth skip). This module never touches the network.
//
// Resolution order (first hit wins):
//   1. explicit `--plugin-source <dir>` (treated as a monorepo root that
//      contains `packages/bridge` + `packages/verify`)
//   2. `UNREAL_OPEN_MCP_ROOT` env var (CI / monorepo checkouts where the CLI is
//      installed outside the repo)
//   3. walk up from the CLI's own location to find a `packages/bridge` dir with
//      `UnrealOpenMCP.uplugin` (a local dev checkout)
//
// When none resolve, the caller surfaces a clear non-zero exit pointing at the
// `--plugin-source` flag.

import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

/** Env var a CI / monorepo checkout can set to pin the repo root. */
export const PLUGIN_ROOT_ENV_VAR = "UNREAL_OPEN_MCP_ROOT";

/** Bridge plugin's install folder name + descriptor (must match `.uplugin`). */
export const BRIDGE_PLUGIN_NAME = "UnrealOpenMCP";
export const BRIDGE_DESCRIPTOR = "UnrealOpenMCP.uplugin";

/** Verify plugin's install folder name + descriptor. */
export const VERIFY_PLUGIN_NAME = "UnrealOpenMCPVerify";
export const VERIFY_DESCRIPTOR = "UnrealOpenMCPVerify.uplugin";

/** Maximum parent-walk depth when probing for a monorepo root. */
const MAX_WALK_DEPTH = 8;

export interface PluginSourceRoots {
  /** Repo / monorepo root containing `packages/bridge` + `packages/verify`. */
  root: string;
  /** Absolute bridge plugin source (`packages/bridge`). */
  bridgeSourceDir: string;
  /** Absolute verify plugin source (`packages/verify`). */
  verifySourceDir: string;
}

export type PluginSourceErrorKind =
  | "explicit_not_found"
  | "env_not_found"
  | "walk_not_found";

export interface PluginSourceError {
  kind: PluginSourceErrorKind;
  message: string;
}

export type ResolvePluginSourceOutcome =
  | ({ ok: true } & PluginSourceRoots)
  | ({ ok: false } & PluginSourceError);

/**
 * True when `root` looks like the Unreal Open MCP monorepo root — i.e. it has
 * `packages/bridge/UnrealOpenMCP.uplugin`. The verify descriptor is checked
 * separately so a missing verify plugin degrades to a warning, not a hard
 * failure (a bridge-only install is valid).
 */
export function isMonorepoRoot(root: string): boolean {
  return fs.existsSync(path.join(root, "packages", "bridge", BRIDGE_DESCRIPTOR));
}

/**
 * Walk up from `startDir` looking for a monorepo root. Returns it, or `null`
 * when the CLI was installed outside the repo (e.g. via npm global install).
 *
 * Adapted from Unreal-MCP's `findRepoRoot` (read-only behavior reference); the
 * probe target is `packages/bridge/UnrealOpenMCP.uplugin` instead of a bare
 * `UnrealMCP/` dir because the Unreal Open MCP layout is a pnpm-style monorepo.
 */
export function findMonorepoRoot(startDir: string): string | null {
  let dir = path.resolve(startDir);
  for (let i = 0; i < MAX_WALK_DEPTH; i++) {
    if (isMonorepoRoot(dir)) return dir;
    const parent = path.dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }
  return null;
}

/**
 * Resolve the plugin source roots. Never throws — failures are structured
 * `ok:false` outcomes so the CLI surfaces a clean message + exit code.
 *
 * @param explicitRoot  `--plugin-source <dir>` value, if any.
 * @param startUrl      Module URL to walk up from for the default probe
 *                      (default: `import.meta.url` of the caller — injectable
 *                      for tests).
 * @param envRoot       Env override (default: `process.env[PLUGIN_ROOT_ENV_VAR]`).
 */
export function resolvePluginSource(
  explicitRoot: string | undefined,
  startUrl: string = import.meta.url,
  envRoot: string | undefined = process.env[PLUGIN_ROOT_ENV_VAR],
): ResolvePluginSourceOutcome {
  // 1. explicit --plugin-source
  if (explicitRoot) {
    const root = path.resolve(explicitRoot);
    if (!isMonorepoRoot(root)) {
      return {
        ok: false,
        kind: "explicit_not_found",
        message:
          `--plugin-source '${explicitRoot}' does not look like the Unreal Open MCP ` +
          `monorepo root (expected packages/bridge/${BRIDGE_DESCRIPTOR}).`,
      };
    }
    return okRoots(root);
  }

  // 2. UNREAL_OPEN_MCP_ROOT env
  if (envRoot) {
    const root = path.resolve(envRoot);
    if (!isMonorepoRoot(root)) {
      return {
        ok: false,
        kind: "env_not_found",
        message:
          `$${PLUGIN_ROOT_ENV_VAR}='${envRoot}' does not look like the Unreal Open MCP ` +
          `monorepo root (expected packages/bridge/${BRIDGE_DESCRIPTOR}).`,
      };
    }
    return okRoots(root);
  }

  // 3. walk up from this module's location
  const here = path.dirname(fileURLToPath(startUrl));
  const root = findMonorepoRoot(here);
  if (!root) {
    return {
      ok: false,
      kind: "walk_not_found",
      message:
        `Could not locate the Unreal Open MCP monorepo root from ${here}. ` +
        `Pass --plugin-source <dir> or set $${PLUGIN_ROOT_ENV_VAR}.`,
    };
  }
  return okRoots(root);
}

function okRoots(root: string): { ok: true } & PluginSourceRoots {
  return {
    ok: true,
    root,
    bridgeSourceDir: path.join(root, "packages", "bridge"),
    verifySourceDir: path.join(root, "packages", "verify"),
  };
}

/**
 * True when a verify plugin source is present at the resolved root. Used by the
 * installer to decide whether `--with-verify` (default on) can be honored or
 * must degrade to a warning.
 */
export function hasVerifySource(roots: PluginSourceRoots): boolean {
  return fs.existsSync(path.join(roots.verifySourceDir, VERIFY_DESCRIPTOR));
}
