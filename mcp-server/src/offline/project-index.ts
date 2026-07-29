// Offline project-index reader for `unreal_open_mcp_project_index`.
//
// Returns `.uproject` basics (name, modules, engine association) + an optional
// bounded file listing under allow-listed project roots (`Source/`, `Config/`,
// `Content/` directory names only — never binary content reads). This is the
// minimal introspection an agent gets when the bridge is dead: enough to orient
// (project name, modules, engine version, source tree shape, config files)
// without claiming `.uasset` parity (ADR-006 — no offline asset graph).
//
// Adapted from Unity's offline inventory ergonomics (scan-paths shape) for the
// file-tree listing; the `.uproject` parser is greenfield (Unity has no
// `.uproject` equivalent — it uses ProjectSettings/*.asset binaries). No new
// deps beyond node built-ins.

import { existsSync, readFileSync, readdirSync, statSync, realpathSync } from "node:fs";
import { join, relative, sep } from "node:path";

/** A module declared in the `.uproject` Modules array. */
export interface UProjectModule {
  /** Module name (e.g. "MyGame"). */
  name: string;
  /** Module type if present (e.g. "Runtime", "Editor"). */
  type?: string;
  /** Loading phase if present (e.g. "Default", "PreDefault"). */
  loading_phase?: string;
}

/** Parsed `.uproject` descriptor. Best-effort — unknown fields are ignored. */
export interface UProjectDescriptor {
  /** Schema version (EnterprisePlugin / 3 / ...). */
  epoch?: number;
  /** AssociationToken from EngineAssociation (e.g. "5.8", "5.6", "{GUID}"). */
  engine_association?: string;
  /** Category (e.g. "Project"). */
  category?: string;
  /** Description string. */
  description?: string;
  /** Declared modules. */
  modules: UProjectModule[];
  /** Enabled plugins (name only). */
  plugins: string[];
  /** True when the file was found and parsed (false → missing/unparseable). */
  found: boolean;
  /** Raw parse error when the file existed but was not valid JSON. */
  parse_error?: string;
}

/**
 * Parse a `.uproject` descriptor. Best-effort: a missing file → `{ found:false }`;
 * an unparseable file → `{ found:true, parse_error }`; otherwise the fields an
 * agent needs to orient (engine association, modules, enabled plugins). Never
 * throws.
 */
export function parseUProject(projectPath: string): UProjectDescriptor {
  const file = join(projectPath, `${basename(projectPath)}.uproject`);
  // The .uproject is named after the project directory. If that exact name is
  // absent, fall back to the first .uproject in the project root (a project
  // dir may have been renamed).
  let uprojectPath = file;
  if (!existsSync(file)) {
    let fallback: string | null = null;
    try {
      for (const name of readdirSync(projectPath)) {
        if (name.endsWith(".uproject")) {
          fallback = join(projectPath, name);
          break;
        }
      }
    } catch {
      fallback = null;
    }
    if (!fallback) return { found: false, modules: [], plugins: [] };
    uprojectPath = fallback;
  }

  let content: string;
  try {
    content = readFileSync(uprojectPath, "utf8");
  } catch (err) {
    return {
      found: true,
      modules: [],
      plugins: [],
      parse_error: err instanceof Error ? err.message : String(err),
    };
  }

  let raw: unknown;
  try {
    raw = JSON.parse(content);
  } catch (err) {
    return {
      found: true,
      modules: [],
      plugins: [],
      parse_error: err instanceof Error ? err.message : String(err),
    };
  }
  if (!raw || typeof raw !== "object") {
    return { found: true, modules: [], plugins: [], parse_error: "not a JSON object" };
  }
  const obj = raw as Record<string, unknown>;
  const modules: UProjectModule[] = [];
  if (Array.isArray(obj.Modules)) {
    for (const m of obj.Modules) {
      if (!m || typeof m !== "object") continue;
      const mo = m as Record<string, unknown>;
      const entry: UProjectModule = {
        name: typeof mo.Name === "string" ? mo.Name : "",
      };
      if (typeof mo.Type === "string") entry.type = mo.Type;
      if (typeof mo.LoadingPhase === "string") entry.loading_phase = mo.LoadingPhase;
      if (entry.name) modules.push(entry);
    }
  }
  const plugins: string[] = [];
  if (Array.isArray(obj.Plugins)) {
    for (const p of obj.Plugins) {
      if (!p || typeof p !== "object") continue;
      const po = p as Record<string, unknown>;
      if (typeof po.Name === "string") plugins.push(po.Name);
    }
  }
  return {
    found: true,
    modules,
    plugins,
    ...(typeof obj.Epoch === "number" ? { epoch: obj.Epoch } : {}),
    ...(typeof obj.EngineAssociation === "string"
      ? { engine_association: obj.EngineAssociation }
      : {}),
    ...(typeof obj.Category === "string" ? { category: obj.Category } : {}),
    ...(typeof obj.Description === "string" ? { description: obj.Description } : {}),
  };
}

/** The last path segment of `p` (a portable basename without node:path's
 *  platform quirks for the `.uproject` name derivation). */
function basename(p: string): string {
  const norm = p.replace(/\\/g, "/").replace(/\/+$/, "");
  const slash = norm.lastIndexOf("/");
  return slash >= 0 ? norm.slice(slash + 1) : norm;
}

/** Allow-listed directory names under the project root that a file listing may
 *  walk. Names only — never binary content reads (ADR-006). */
const ALLOW_LIST_ROOTS = new Set(["Source", "Config", "Content"]);

/** File extensions surfaced in the listing (source + config text; no binaries). */
const LISTING_EXTENSIONS = new Set([
  ".h",
  ".hpp",
  ".c",
  ".cc",
  ".cpp",
  ".cs",
  ".ini",
  ".txt",
  ".Build.cs",
]);

/** Hard cap on the number of files a listing returns. Keeps the payload bounded
 *  for a large project tree. */
const DEFAULT_MAX_FILES = 200;

export interface FileListEntry {
  /** Path relative to the project root (forward slashes). */
  path: string;
  /** File size in bytes. */
  bytes: number;
}

export interface ProjectIndexFileList {
  files: FileListEntry[];
  /** True when the listing hit the max_files cap (more files exist). */
  truncated: boolean;
}

/**
 * List files under an allow-listed root (`Source` / `Config` / `Content`) in the
 * project, bounded by `maxFiles`. Only text extensions are surfaced
 * (`.h/.hpp/.c/.cc/.cpp/.cs/.ini/.txt` + `Build.cs`); binary assets
 * (`.uasset`/`.umap`) are NEVER read or listed here — that is the ADR-006
 * offline-asset limit. Returns an empty list when the root is absent.
 *
 * The listing never escapes the project root: a symlink/junction whose
 * resolved target is outside the project is skipped (best-effort containment
 * re-check against the resolved path).
 */
export function listProjectFiles(
  projectPath: string,
  root: string,
  opts: { recursive?: boolean; max_files?: number } = {},
): ProjectIndexFileList {
  if (!ALLOW_LIST_ROOTS.has(root)) {
    return { files: [], truncated: false };
  }
  const recursive = opts.recursive !== false;
  const maxFiles = Math.max(1, Math.floor(opts.max_files ?? DEFAULT_MAX_FILES));
  const rootAbs = join(projectPath, root);
  const files: FileListEntry[] = [];
  let truncated = false;

  if (!existsSync(rootAbs)) return { files, truncated };

  const walk = (dir: string): void => {
    if (truncated) return;
    let entries: string[];
    try {
      entries = readdirSync(dir);
    } catch {
      return;
    }
    for (const name of entries) {
      if (files.length >= maxFiles) {
        truncated = true;
        return;
      }
      const full = join(dir, name);
      let st;
      try {
        st = statSync(full);
      } catch {
        continue;
      }
      if (st.isDirectory()) {
        if (recursive) walk(full);
      } else if (st.isFile()) {
        if (hasListedExtension(name)) {
          // Containment re-check against the REAL resolved path so a symlink /
          // junction pointing OUTSIDE the project root is skipped (its on-disk
          // target resolves outside, even though the walked path stays inside).
          // A leading `..` or an absolute relative result is an escape.
          let real: string;
          try {
            real = realpathSync(full);
          } catch {
            continue;
          }
          const realProject = realpathSync(projectPath);
          const rel = relative(realProject, real);
          if (rel.startsWith("..") || isAbsolutePortable(rel)) continue;
          files.push({ path: rel.split(sep).join("/"), bytes: st.size });
        }
      }
    }
  };
  walk(rootAbs);

  // Stable ordering: path ascending.
  files.sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
  return { files, truncated };
}

/** Does `name` end with a listed extension (case-insensitive on the suffix)? */
function hasListedExtension(name: string): boolean {
  const lower = name.toLowerCase();
  // Build.cs is a compound suffix; check it before the single-dot extensions.
  if (lower.endsWith(".build.cs")) return true;
  const dot = lower.lastIndexOf(".");
  if (dot < 0) return false;
  return LISTING_EXTENSIONS.has(lower.slice(dot));
}

/** Portable isAbsolute check (handles a Windows drive-letter relative result on
 *  any host). */
function isAbsolutePortable(p: string): boolean {
  if (!p) return false;
  return p.startsWith("/") || /^[A-Za-z]:[\\/]/.test(p);
}

export interface ProjectIndexResult {
  /** The project root the index was built from. */
  projectPath: string;
  /** Parsed `.uproject` descriptor (found:false when absent). */
  uproject: UProjectDescriptor;
  /** Optional file listing under an allow-listed root (omitted when no `list`
   * arg). */
  file_list?: ProjectIndexFileList;
}

/**
 * Build the project index: `.uproject` basics + an optional file listing. The
 * `.uproject` parse is always performed; the file list runs only when `list` is
 * a valid allow-listed root. Never throws.
 */
export function buildProjectIndex(
  projectPath: string,
  opts: { list?: string; recursive?: boolean; max_files?: number } = {},
): ProjectIndexResult {
  const uproject = parseUProject(projectPath);
  const result: ProjectIndexResult = { projectPath, uproject };
  if (opts.list && ALLOW_LIST_ROOTS.has(opts.list)) {
    result.file_list = listProjectFiles(projectPath, opts.list, {
      recursive: opts.recursive,
      max_files: opts.max_files,
    });
  }
  return result;
}
