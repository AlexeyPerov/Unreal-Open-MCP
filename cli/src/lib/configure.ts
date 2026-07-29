// `configure` — read/write the project-local Open MCP settings file.
//
// Adapted (adapt fidelity) from Unity Open MCP's
// `<project>/.unity-open-mcp/settings.json` persistence shape and Unreal-MCP's
// `lib/configure.ts` layout. Deltas (per P8.5 plan):
//   - Local-only. No cloud host / cloud URL / connection-mode / cloud-token
//     fields (ADR-001 skip). The only connection knob in the MVP is the bridge
//     port override.
//   - JSON settings file, not a `.env`. Unity Open MCP already uses
//     `<project>/.unity-open-mcp/settings.json`; the Unreal bridge already
//     reads `authMode` from `<project>/.unreal-open-mcp/settings.json`
//     (docs/architecture.md), so configure writes there to keep a single
//     settings source — never invents a parallel `.env`.
//   - No secrets in the MVP. Auth tokens live on the per-session instance lock
//     (P5.6); configure does not write tokens.
//
// Merge contract: a re-run deep-merges the patch into the existing settings
// (unrelated keys are preserved byte-for-byte in insertion order). A missing
// or malformed file is treated as empty (a fresh object) so the command never
// clobbers a user's hand-edited file with bad data — it starts fresh.
//
// No external deps (only node:fs, node:path).

import * as fs from "node:fs";
import { join } from "node:path";

import { STATUS_DIR_NAME } from "./port-discovery.js";

/**
 * The project-local settings directory (the same one the bridge reads
 * `authMode` from). Co-located with the project so settings travel with it;
 * distinct from {@link statusDir} (home) which holds the instance locks.
 */
export function settingsDir(projectDir: string): string {
  return join(projectDir, STATUS_DIR_NAME);
}

/** Absolute path to the project's settings file. */
export function settingsPath(projectDir: string): string {
  return join(settingsDir(projectDir), "settings.json");
}

/**
 * The shape of the settings file. Only the keys `configure` manages are typed
 * here; the bridge writes more (authMode, defaultGateMode, ...). Unknown keys
 * are preserved across a write (deep-merge).
 */
export interface ProjectSettings {
  /**
   * Optional bridge port override. When set, the bridge binds this port
   * instead of its deterministic per-project hash. `null` clears an explicit
   * override (the bridge falls back to the hash). Omitted = leave unchanged.
   */
  bridgePort?: number | null;
  /** Any other keys the bridge / a future command writes are preserved. */
  [key: string]: unknown;
}

/** A patch applied to the settings file. Same shape as {@link ProjectSettings}. */
export type SettingsPatch = ProjectSettings;

/** Result of a settings read. */
export interface ReadSettingsResult {
  ok: true;
  /** The parsed settings (an empty object when the file is missing/malformed). */
  settings: ProjectSettings;
  /** Absolute path that was read. */
  path: string;
  /** True when the file did not exist (a fresh empty object was returned). */
  missing: boolean;
  /** True when the file existed but did not parse (treated as empty). */
  malformed: boolean;
}

/** Result of a settings write. */
export interface WriteSettingsResult {
  ok: true;
  /** The merged settings now persisted. */
  settings: ProjectSettings;
  /** Absolute path that was written. */
  path: string;
  /** True when the settings directory had to be created. */
  createdDir: boolean;
}

/** Error kinds `configure` can surface. */
export type ConfigureErrorKind =
  | "project_dir_missing" // projectDir does not exist
  | "project_dir_not_dir" // projectDir is not a directory
  | "read_failed" // settings file existed but could not be read
  | "write_failed"; // the atomic write failed

export interface ConfigureError {
  ok: false;
  kind: ConfigureErrorKind;
  message: string;
  path?: string;
}

/**
 * Read the project's settings file. A missing file is a success with an empty
 * object + `missing:true` (configure is idempotent — a first run starts fresh).
 * A malformed file is also a success with an empty object + `malformed:true` so
 * a write never clobbers a user's hand-edited file with garbage data — the
 * patch is merged onto a clean slate.
 */
export function readSettings(projectDir: string): ReadSettingsResult | ConfigureError {
  if (!fs.existsSync(projectDir)) {
    return {
      ok: false,
      kind: "project_dir_missing",
      message: `Project directory does not exist: ${projectDir}`,
    };
  }
  const stat = fs.statSync(projectDir);
  if (!stat.isDirectory()) {
    return {
      ok: false,
      kind: "project_dir_not_dir",
      message: `Project path is not a directory: ${projectDir}`,
    };
  }
  const p = settingsPath(projectDir);
  if (!fs.existsSync(p)) {
    return { ok: true, settings: {}, path: p, missing: true, malformed: false };
  }
  let raw: string;
  try {
    raw = fs.readFileSync(p, "utf8");
  } catch {
    return {
      ok: false,
      kind: "read_failed",
      message: `Could not read settings file: ${p}`,
      path: p,
    };
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return { ok: true, settings: {}, path: p, missing: false, malformed: true };
  }
  // A non-object JSON value (array, string, number) is treated as malformed.
  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
    return { ok: true, settings: {}, path: p, missing: false, malformed: true };
  }
  return {
    ok: true,
    settings: parsed as ProjectSettings,
    path: p,
    missing: false,
    malformed: false,
  };
}

/**
 * Deep-merge a patch into the existing settings and persist atomically. Unknown
 * keys in the existing file are preserved (insertion order kept); keys in the
 * patch overwrite. A patch value of `null` clears the key (deletes it from the
 * file) so `--clear-bridge-port` removes the entry rather than leaving a null.
 *
 * Returns the merged settings + the written path. Never throws — I/O failures
 * surface as a structured {@link ConfigureError}.
 */
export function writeSettings(
  projectDir: string,
  patch: SettingsPatch,
): WriteSettingsResult | ConfigureError {
  const read = readSettings(projectDir);
  if (!read.ok) return read;

  const merged = mergeSettings(read.settings, patch);

  const dir = settingsDir(projectDir);
  let createdDir = false;
  if (!fs.existsSync(dir)) {
    try {
      fs.mkdirSync(dir, { recursive: true });
      createdDir = true;
    } catch {
      return {
        ok: false,
        kind: "write_failed",
        message: `Could not create settings directory: ${dir}`,
        path: dir,
      };
    }
  }

  const body = JSON.stringify(merged, null, 2) + "\n";
  const tmp = `${read.path}.${process.pid}.tmp`;
  try {
    fs.writeFileSync(tmp, body, "utf8");
    fs.renameSync(tmp, read.path);
  } catch {
    // Best-effort cleanup of the temp file on failure.
    try {
      fs.unlinkSync(tmp);
    } catch {
      // ignore
    }
    return {
      ok: false,
      kind: "write_failed",
      message: `Could not write settings file: ${read.path}`,
      path: read.path,
    };
  }

  return { ok: true, settings: merged, path: read.path, createdDir };
}

/**
 * Merge a patch into existing settings. Pure. A patch value of `null` deletes
 * the key (the caller's "clear" intent). Nested plain objects are merged
 * recursively; everything else is overwritten.
 */
export function mergeSettings(
  existing: ProjectSettings,
  patch: SettingsPatch,
): ProjectSettings {
  const out: ProjectSettings = { ...existing };
  for (const [key, value] of Object.entries(patch)) {
    if (value === null) {
      delete out[key];
      continue;
    }
    if (
      value !== null &&
      typeof value === "object" &&
      !Array.isArray(value) &&
      typeof out[key] === "object" &&
      out[key] !== null &&
      !Array.isArray(out[key])
    ) {
      out[key] = mergeSettings(
        out[key] as ProjectSettings,
        value as ProjectSettings,
      );
    } else {
      out[key] = value;
    }
  }
  return out;
}
