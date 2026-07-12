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

/** @param {string} body @param {string} v */
function setJsonVersion(body, v) {
  return body.replace(
    /("version"\s*:\s*")[^"]*(")/,
    (_, pre, post) => `${pre}${v}${post}`,
  );
}

/**
 * Replace the C++ BRIDGE_VERSION constant in UnrealOpenMcpBridgeSession.h.
 * Matches `BRIDGE_VERSION = TEXT("<any>")` and rewrites the quoted string.
 * @param {string} body @param {string} v
 */
function setCppBridgeVersion(body, v) {
  return body.replace(
    /(BRIDGE_VERSION\s*=\s*TEXT\(")[^"]*("\))/,
    (_, pre, post) => `${pre}${v}${post}`,
  );
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
 * @returns {{ changed: Array, drifted: Array, missing: Array }}
 */
function syncTargets(sourceFile, targets, mode) {
  const want = readSourceVersion(sourceFile);
  /** @type {Array<{file:string, description:string, from?:string, to:string}>} */
  const changed = [];
  /** @type {Array<{file:string, description:string, from:string, want:string}>} */
  const drifted = [];
  /** @type {Array<{file:string, description:string}>} */
  const missing = [];

  for (const t of targets) {
    const p = abs(t.file);
    if (!existsSync(p)) {
      missing.push({ file: t.file, description: t.description });
      continue;
    }
    const original = readFileSync(p, "utf8");
    const updated = t.replace(original, want);
    if (updated === original) continue;
    const from = extractVersion(original, t.kind);
    if (mode === "write") {
      writeFileSync(p, updated);
      changed.push({ file: t.file, description: t.description, from, to: want });
    } else {
      drifted.push({ file: t.file, description: t.description, from, want });
    }
  }
  return { changed, drifted, missing };
}

/** @param {string} body @param {string} kind @returns {string | undefined} */
function extractVersion(body, kind) {
  if (kind === "cpp") {
    const m = body.match(/BRIDGE_VERSION\s*=\s*TEXT\("([^"]+)"\)/);
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

if (mode === "write") {
  if (result.changed.length === 0 && result.missing.length === 0) {
    console.log(`shared trio: already in sync at ${readSourceVersion(TRIO_SOURCE)}.`);
  } else {
    console.log(`shared trio: synced to ${readSourceVersion(TRIO_SOURCE)}.`);
    for (const c of result.changed) {
      console.log(`  ${c.file}${c.from ? ` (${c.from} → ${c.to})` : ""}`);
    }
  }
  for (const m of result.missing) {
    console.warn(`  ⚠  missing target: ${m.file} (${m.description})`);
  }
  process.exit(0);
}

if (result.drifted.length === 0) {
  console.log(`shared trio: OK (all targets match ${readSourceVersion(TRIO_SOURCE)}).`);
  for (const m of result.missing) {
    console.warn(`  ⚠  missing target: ${m.file} (${m.description})`);
  }
  process.exit(0);
}

console.error(`✖ shared trio version drift detected. Source ${TRIO_SOURCE} = ${readSourceVersion(TRIO_SOURCE)}.`);
for (const d of result.drifted) {
  console.error(`  ${d.file}: ${d.from ?? "<unmatched>"} (expected ${d.want})`);
}
console.error("\nFix: run `node scripts/sync-version.mjs` from the repo root.");
process.exit(1);
