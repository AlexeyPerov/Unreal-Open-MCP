// `.uproject` descriptor read / merge / write.
//
// Adapted from Unity Open MCP's offline-read philosophy (parse-on-disk JSON
// without launching the editor) and Unreal-MCP's `utils/project.ts` shape, but
// narrowed to the single contract `install-plugin` needs: enable a plugin in
// the descriptor's `Plugins` array idempotently, preserving every unrelated
// field and — where practical — key order, so a re-install never corrupts or
// reflows the user's `.uproject`.
//
// The merge is intentionally lossless and minimal:
//   - parse with JSON.parse (descriptor is strict JSON; UE rejects comments).
//   - upsert `{ Name, Enabled }` by exact `Name` (single source of truth = the
//     `.uplugin` descriptor's own `Name`-equivalent folder, which the caller
//     passes in — never invented here).
//   - re-serialize with a 2-space indent + trailing newline to match the
//     convention Unreal itself writes. We do NOT hand-roll a JSON printer that
//     preserves insertion order across all keys; Node's `JSON.stringify` already
//     preserves own-property insertion order for plain objects parsed from JSON,
//     which is sufficient for `.uproject` files authored by UE.

import * as fs from "node:fs";
import * as path from "node:path";

/** A single entry in the descriptor's top-level `Plugins` array. */
export interface UProjectPluginEntry {
  Name: string;
  Enabled: boolean;
  [k: string]: unknown;
}

/** Loose shape of a parsed `.uproject` (UE adds fields over time). */
export interface UProjectDescriptor {
  EngineAssociation?: string;
  Plugins?: UProjectPluginEntry[];
  Modules?: unknown[];
  [k: string]: unknown;
}

/** Result of a successful read. */
export interface ReadUProjectResult {
  /** Absolute path to the `.uproject` file. */
  uprojectPath: string;
  /** Absolute project root (the `.uproject`'s directory). */
  projectDir: string;
  /** Parsed descriptor (mutable; the caller may edit then `writeUProject`). */
  descriptor: UProjectDescriptor;
}

export type ReadUProjectErrorKind =
  | "not_found" // no .uproject in the directory
  | "read_failed" // exists but unreadable (permissions, I/O)
  | "parse_failed"; // unreadable / invalid JSON

export interface ReadUProjectError {
  kind: ReadUProjectErrorKind;
  message: string;
}

export type ReadUProjectOutcome =
  | ({ ok: true } & ReadUProjectResult)
  | ({ ok: false } & ReadUProjectError);

/**
 * Find the single `.uproject` directly inside `projectDir`. Returns the
 * absolute path, or `null` when none (or, deterministically, the first
 * alphabetically when several — UE itself only supports one per folder).
 *
 * Adapted from Unreal-MCP's `findUProjectFile` (read-only behavior reference).
 */
export function findUProjectFile(projectDir: string): string | null {
  let entries: string[];
  try {
    entries = fs.readdirSync(projectDir);
  } catch {
    return null;
  }
  const uprojects = entries
    .filter((e) => e.toLowerCase().endsWith(".uproject"))
    .sort((a, b) => a.localeCompare(b));
  if (uprojects.length === 0) return null;
  return path.join(projectDir, uprojects[0]);
}

/** True when `projectDir` contains a `.uproject`. Pure-ish (one readdir). */
export function isUnrealProjectDir(projectDir: string): boolean {
  return findUProjectFile(projectDir) !== null;
}

/**
 * Read + parse the `.uproject` inside `projectDir`. Never throws — failures are
 * reported as a structured `ok:false` outcome so the caller can surface a clean
 * CLI error instead of a stack trace.
 */
export function readUProject(projectDir: string): ReadUProjectOutcome {
  const uprojectPath = findUProjectFile(projectDir);
  if (!uprojectPath) {
    return {
      ok: false,
      kind: "not_found",
      message: `No .uproject file found in ${projectDir}.`,
    };
  }

  let body: string;
  try {
    body = fs.readFileSync(uprojectPath, "utf8");
  } catch (e) {
    return {
      ok: false,
      kind: "read_failed",
      message: `Could not read ${uprojectPath}: ${(e as Error).message}`,
    };
  }

  let descriptor: UProjectDescriptor;
  try {
    descriptor = JSON.parse(body) as UProjectDescriptor;
  } catch (e) {
    return {
      ok: false,
      kind: "parse_failed",
      message: `Could not parse ${uprojectPath} as JSON: ${(e as Error).message}`,
    };
  }

  if (!descriptor || typeof descriptor !== "object" || Array.isArray(descriptor)) {
    return {
      ok: false,
      kind: "parse_failed",
      message: `${uprojectPath} does not contain a JSON object at the top level.`,
    };
  }

  return {
    ok: true,
    uprojectPath,
    projectDir: path.dirname(uprojectPath),
    descriptor,
  };
}

/**
 * Find the index of a plugin entry by exact `Name`. Returns -1 when absent or
 * when the `Plugins` array is missing / not an array. Pure.
 */
export function findPluginEntryIndex(
  descriptor: UProjectDescriptor,
  pluginName: string,
): number {
  const plugins = descriptor.Plugins;
  if (!Array.isArray(plugins)) return -1;
  return plugins.findIndex(
    (e): e is UProjectPluginEntry =>
      !!e && typeof e === "object" && !Array.isArray(e) && (e as UProjectPluginEntry).Name === pluginName,
  );
}

export interface UpsertPluginOptions {
  /**
   * Extra fields to set on the entry (e.g. `MarketplaceURL`, `Enabled` is always
   * set explicitly). Merged shallowly; never used to override `Name`.
   */
  extra?: Record<string, unknown>;
}

export interface UpsertPluginResult {
  /** The resulting entry (the upserted object inside `descriptor.Plugins`). */
  entry: UProjectPluginEntry;
  /** True when a NEW entry was appended; false when an existing one was edited. */
  inserted: boolean;
}

/**
 * Idempotently upsert a `{ Name, Enabled: true }` entry in the descriptor's
 * `Plugins` array, in place. Returns whether a new entry was inserted (a no-op
 * re-enable returns `inserted:false` so the caller can skip the file write).
 *
 * `Name` is the single source of truth for plugin identity in a `.uproject` and
 * MUST match the installed plugin's folder name + the `.uplugin`'s implied
 * identity. The caller resolves it from the `.uplugin` / install dir, never
 * inventing it here.
 */
export function upsertPluginEntry(
  descriptor: UProjectDescriptor,
  pluginName: string,
  opts: UpsertPluginOptions = {},
): UpsertPluginResult {
  if (!Array.isArray(descriptor.Plugins)) {
    descriptor.Plugins = [];
  }
  const idx = findPluginEntryIndex(descriptor, pluginName);
  if (idx >= 0) {
    const entry = descriptor.Plugins[idx] as UProjectPluginEntry;
    entry.Enabled = true;
    if (opts.extra) {
      for (const [k, v] of Object.entries(opts.extra)) {
        if (k === "Name") continue; // never override identity
        entry[k] = v;
      }
    }
    return { entry, inserted: false };
  }
  const entry: UProjectPluginEntry = { Name: pluginName, Enabled: true };
  if (opts.extra) {
    for (const [k, v] of Object.entries(opts.extra)) {
      if (k === "Name") continue;
      entry[k] = v;
    }
  }
  descriptor.Plugins.push(entry);
  return { entry, inserted: true };
}

/**
 * Serialize a descriptor back to the on-disk `.uproject` form: 2-space indent
 * (matches UE's own writer) + a trailing newline. Pure (no I/O).
 */
export function serializeUProject(descriptor: UProjectDescriptor): string {
  // JSON.stringify preserves own-property insertion order for objects parsed
  // from JSON, so existing keys keep their position. Appended `Plugins` entries
  // land at the end of their array, which is the least-surprising placement.
  return JSON.stringify(descriptor, null, 2) + "\n";
}

/**
 * Write the descriptor atomically-ish: write to a `.tmp` sibling then rename
 * over the target. Halves the window during which a crash or signal could
 * leave a half-written descriptor. Throws on I/O failure — the caller wraps it.
 */
export function writeUProject(
  descriptor: UProjectDescriptor,
  uprojectPath: string,
): void {
  const body = serializeUProject(descriptor);
  const tmp = `${uprojectPath}.${process.pid}.tmp`;
  fs.writeFileSync(tmp, body, "utf8");
  fs.renameSync(tmp, uprojectPath);
}
