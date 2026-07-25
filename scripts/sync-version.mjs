#!/usr/bin/env node
// sync-version.mjs — single-source-of-truth version sync for Unreal Open MCP.
//
// The SHARED TRIO version (npm MCP server + bridge plugin + verify module).
// Source of truth: <repo>/version.json. These three ship breaking changes
// together and must stay on the same number.
//
// Every other place a version string appears is GENERATED from version.json
// by this script. Never hand-edit a generated target — bump the source and
// run `node scripts/sync-version.mjs`. The CI gate (version-sync.yml) fails
// any PR where a generated target has drifted from its source.
//
// Unreal-specific targets:
//   - mcp-server/package.json
//   - packages/bridge/package.json
//   - packages/verify/package.json
//   - packages/bridge/Source/.../Bridge/UnrealOpenMcpBridgeSession.h (BRIDGE_VERSION constant)
//
// Usage:
//   node scripts/sync-version.mjs                # rewrite all trio targets from version.json
//   node scripts/sync-version.mjs --check        # read-only; exit 1 if any trio target drifted
//   node scripts/sync-version.mjs bump <level>   # bump version.json + sync trio
//   node scripts/sync-version.mjs set <X.Y.Z>      # set version.json to <X.Y.Z> + sync trio
//
//   <level> = major | minor | patch
//
// Requires Node 18+ (no runtime dependencies, only node: builtins).

import { readFileSync, writeFileSync, existsSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const REPO_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");

const TRIO_SOURCE = "version.json";

/**
 * Apply a version regex, reporting whether it matched at all.
 *
 * Every replacer returns { body, matched } so syncTargets can distinguish
 * "already in sync" from "the pattern no longer matches this file". Previously
 * both looked identical (`updated === original`), so renaming or reformatting a
 * version declaration made the --check gate silently report OK forever.
 *
 * @param {string} body @param {RegExp} re @param {string} v
 * @returns {{ body: string, matched: boolean }}
 */
function applyVersionRegex(body, re, v) {
  let matched = false;
  const out = body.replace(re, (_, pre, post) => {
    matched = true;
    return `${pre}${v}${post}`;
  });
  return { body: out, matched };
}

/** @param {string} body @param {string} v */
function setJsonVersion(body, v) {
  return applyVersionRegex(body, /("version"\s*:\s*")[^"]*(")/, v);
}

/**
 * Replace the `VersionName` field in a .uplugin descriptor. Distinct from
 * setJsonVersion because a .uplugin's `"Version"` is an integer build counter
 * and its human-readable string lives in `"VersionName"`.
 * @param {string} body @param {string} v
 */
function setUpluginVersionName(body, v) {
  return applyVersionRegex(body, /("VersionName"\s*:\s*")[^"]*(")/, v);
}

/**
 * Replace the C++ BRIDGE_VERSION constant in UnrealOpenMcpBridgeSession.h.
 * Matches `BRIDGE_VERSION = TEXT("<any>")` and rewrites the quoted string.
 * @param {string} body @param {string} v
 */
function setCppBridgeVersion(body, v) {
  return applyVersionRegex(body, /(BRIDGE_VERSION\s*=\s*TEXT\(")[^"]*("\))/, v);
}

const TRIO_TARGETS = [
  {
    file: "mcp-server/package.json",
    kind: "json",
    description: "npm MCP server package.json",
    replace: (b, v) => setJsonVersion(b, v),
  },
  {
    file: "packages/bridge/package.json",
    kind: "json",
    description: "bridge Unreal plugin package.json",
    replace: (b, v) => setJsonVersion(b, v),
  },
  {
    file: "packages/verify/package.json",
    kind: "json",
    description: "verify module package.json",
    replace: (b, v) => setJsonVersion(b, v),
  },
  {
    file: "packages/bridge/Source/UnrealOpenMcpEditor/Private/Bridge/UnrealOpenMcpBridgeSession.h",
    kind: "cpp",
    description: "C++ BRIDGE_VERSION constant (UnrealOpenMcpBridgeSession.h)",
    replace: (b, v) => setCppBridgeVersion(b, v),
  },
  // The .uplugin descriptors carry a user-visible VersionName that the header
  // claims is generated from version.json but which was never in this list — it
  // would silently drift on the next bump.
  {
    file: "packages/bridge/UnrealOpenMCP.uplugin",
    kind: "uplugin",
    description: "bridge plugin descriptor VersionName",
    replace: (b, v) => setUpluginVersionName(b, v),
  },
  {
    file: "packages/verify/UnrealOpenMCPVerify.uplugin",
    kind: "uplugin",
    description: "verify plugin descriptor VersionName",
    replace: (b, v) => setUpluginVersionName(b, v),
  },
];

/** @param {string} rel @returns {string} */
function abs(rel) {
  return resolve(REPO_ROOT, rel);
}

/** @param {string} rel @returns {string} */
function read(rel) {
  return readFileSync(abs(rel), "utf8");
}

/** @param {string} sourceFile @returns {string} */
function readSourceVersion(sourceFile) {
  const body = read(sourceFile);
  const parsed = JSON.parse(body);
  if (typeof parsed.version !== "string" || !parsed.version) {
    throw new Error(`No "version" string in ${sourceFile}`);
  }
  return parsed.version;
}

/**
 * @param {string} sourceFile
 * @param {Array} targets
 * @param {"write"|"check"} mode
 * @returns {{ changed: Array, drifted: Array, missing: Array, unmatched: Array }}
 */
function syncTargets(sourceFile, targets, mode) {
  const want = readSourceVersion(sourceFile);
  /** @type {Array<{file:string, description:string, from?:string, to:string}>} */
  const changed = [];
  /** @type {Array<{file:string, description:string, from:string, want:string}>} */
  const drifted = [];
  /** @type {Array<{file:string, description:string}>} */
  const missing = [];
  /** @type {Array<{file:string, description:string}>} */
  const unmatched = [];

  for (const t of targets) {
    const p = abs(t.file);
    if (!existsSync(p)) {
      missing.push({ file: t.file, description: t.description });
      continue;
    }
    const original = readFileSync(p, "utf8");
    const { body: updated, matched } = t.replace(original, want);
    if (!matched) {
      // The version declaration this target owns is no longer where the regex
      // expects it. That is a broken gate, not a passing one — record it so both
      // modes fail instead of reporting "already in sync".
      unmatched.push({ file: t.file, description: t.description });
      continue;
    }
    if (updated === original) continue;
    const from = extractVersion(original, t.kind);
    if (mode === "write") {
      writeFileSync(p, updated);
      changed.push({ file: t.file, description: t.description, from, to: want });
    } else {
      drifted.push({ file: t.file, description: t.description, from, want });
    }
  }
  return { changed, drifted, missing, unmatched };
}

/** @param {string} body @param {string} kind @returns {string | undefined} */
function extractVersion(body, kind) {
  if (kind === "cpp") {
    const m = body.match(/BRIDGE_VERSION\s*=\s*TEXT\("([^"]+)"\)/);
    return m ? m[1] : undefined;
  }
  if (kind === "uplugin") {
    const m = body.match(/"VersionName"\s*:\s*"([^"]+)"/);
    return m ? m[1] : undefined;
  }
  const m = body.match(/"version"\s*:\s*"([^"]+)"/);
  return m ? m[1] : undefined;
}

/**
 * @param {string} v
 * @param {"major"|"minor"|"patch"} level
 * @returns {string}
 */
function bumpSemver(v, level) {
  const m = /^(\d+)\.(\d+)\.(\d+)/.exec(v);
  if (!m) {
    throw new Error(`Source version ${v} is not X.Y.Z — cannot bump.`);
  }
  let [major, minor, patch] = [Number(m[1]), Number(m[2]), Number(m[3])];
  if (level === "major") {
    major += 1;
    minor = 0;
    patch = 0;
  } else if (level === "minor") {
    minor += 1;
    patch = 0;
  } else {
    patch += 1;
  }
  return `${major}.${minor}.${patch}`;
}

/** @param {string} sourceFile @param {string} newVersion */
function writeSource(sourceFile, newVersion) {
  const original = read(sourceFile);
  const updated = setJsonVersion(original, newVersion);
  if (updated === original) return;
  writeFileSync(abs(sourceFile), updated);
}

const argv = process.argv.slice(2);
const CHECK = argv.includes("--check");
const bumpIdx = argv.indexOf("bump");
const setIdx = argv.indexOf("set");
const isBump = bumpIdx !== -1;
const isSet = setIdx !== -1;
const bumpLevel = isBump ? argv[bumpIdx + 1] : undefined;
const setRaw = isSet ? argv[setIdx + 1] : undefined;

if (isBump && !["major", "minor", "patch"].includes(String(bumpLevel))) {
  console.error("Usage: bump <level> where level is major | minor | patch");
  process.exit(2);
}

const setVersion =
  isSet && typeof setRaw === "string" && /^v?\d+\.\d+\.\d+$/.test(setRaw)
    ? setRaw.replace(/^v/, "")
    : undefined;

if (isSet && setVersion === undefined) {
  console.error("Usage: set <X.Y.Z> where X.Y.Z is plain major.minor.patch");
  process.exit(2);
}

if (isBump && isSet) {
  console.error("bump and set are mutually exclusive.");
  process.exit(2);
}

if (CHECK && (isBump || isSet)) {
  console.error("--check is mutually exclusive with bump and set.");
  process.exit(2);
}

if (isBump || isSet) {
  const current = readSourceVersion(TRIO_SOURCE);
  const next = isBump
    ? bumpSemver(current, /** @type {"major"|"minor"|"patch"} */ (bumpLevel))
    : /** @type {string} */ (setVersion);
  writeSource(TRIO_SOURCE, next);
  const { changed, missing } = syncTargets(TRIO_SOURCE, TRIO_TARGETS, "write");
  const verb = isBump ? "Bumped" : "Set";
  console.log(`${verb} shared trio: ${current} → ${next}`);
  console.log(`  source: ${TRIO_SOURCE}`);
  for (const c of changed) {
    console.log(`  ${c.file}${c.from ? ` (${c.from} → ${c.to})` : ""}`);
  }
  for (const m of missing) {
    console.warn(`  ⚠  missing: ${m.file} (${m.description})`);
  }
  console.log("\nNext: review the diff, then commit and tag.");
  console.log(`  git add -A && git commit -m "chore: bump to ${next}"`);
  console.log(`  git tag v${next} && git push origin v${next}`);
  process.exit(0);
}

const mode = CHECK ? "check" : "write";
const result = syncTargets(TRIO_SOURCE, TRIO_TARGETS, mode);

// A missing file or an unmatched pattern is a BROKEN GATE, not a pass. Both
// modes must surface them and exit non-zero: previously check mode only warned,
// so moving/renaming a target (or reformatting its version declaration) made
// both ci.yml and version-sync.yml silently green forever.
const brokenTargets = [
  ...result.missing.map((m) => ({ ...m, why: "file not found" })),
  ...result.unmatched.map((m) => ({ ...m, why: "version pattern did not match" })),
];

if (mode === "write") {
  if (result.changed.length === 0) {
    console.log(`shared trio: already in sync at ${readSourceVersion(TRIO_SOURCE)}.`);
  } else {
    console.log(`shared trio: synced to ${readSourceVersion(TRIO_SOURCE)}.`);
    for (const c of result.changed) {
      console.log(`  ${c.file}${c.from ? ` (${c.from} → ${c.to})` : ""}`);
    }
  }
}

if (brokenTargets.length > 0) {
  console.error("✖ shared trio target(s) could not be synced:");
  for (const b of brokenTargets) {
    console.error(`  ${b.file}: ${b.why} (${b.description})`);
  }
  console.error(
    "\nFix: restore the target, or update its entry in TRIO_TARGETS " +
      "(scripts/sync-version.mjs).",
  );
  process.exit(1);
}

if (mode === "write") {
  process.exit(0);
}

if (result.drifted.length === 0) {
  console.log(`shared trio: OK (all targets match ${readSourceVersion(TRIO_SOURCE)}).`);
  process.exit(0);
}

console.error(`✖ shared trio version drift detected. Source ${TRIO_SOURCE} = ${readSourceVersion(TRIO_SOURCE)}.`);
for (const d of result.drifted) {
  console.error(`  ${d.file}: ${d.from ?? "<unmatched>"} (expected ${d.want})`);
}
console.error("\nFix: run `node scripts/sync-version.mjs` from the repo root.");
process.exit(1);
