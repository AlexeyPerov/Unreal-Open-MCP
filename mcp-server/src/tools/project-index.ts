import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P8.7 — offline project_index: read `.uproject` basics + an optional file
// listing under allow-listed project roots (Source/ / Config/ / Content/), with
// the bridge DOWN. The minimal introspection an agent gets when the bridge is
// dead: enough to orient (project name, modules, engine association, source
// tree shape, config files) without claiming `.uasset` parity (ADR-006 — no
// offline asset graph).
//
// Route: **offline** (always — resolved from disk; never hits the bridge). The
// router stamps `_source: "offline"` + `_route: { route: "offline" }`.
//
// SCOPE (ADR-006): project files only — `.uproject` descriptor, `Config/*.ini`
// text, `Source/**` text, and a directory-name listing under `Content/`. The
// listing NEVER reads binary `.uasset`/`.umap` content (the file list surfaces
// text-extension files only; binary assets are deferred to a future batch
// commandlet or the live AssetRegistry).
export const projectIndex: Tool = {
  name: "unreal_open_mcp_project_index",
  description:
    "Read project index basics from disk, with the bridge DOWN: parses the " +
    ".uproject descriptor (engine association, declared modules, enabled " +
    "plugins) and optionally lists files under an allow-listed project root " +
    "(Source / Config / Content). Use this when the bridge is unreachable and " +
    "you need to orient — project name, modules, engine version, source tree " +
    "shape, config files. Result shape: { projectPath, uproject: { found, " +
    "engine_association?, modules:[{name,type?,loading_phase?}], plugins:[name] }, " +
    "file_list?: { files:[{path,bytes}], truncated } }. Pass `list` " +
    "('Source' | 'Config' | 'Content') to walk that root (recursive default " +
    "true; max_files default 200) and surface text-extension files only " +
    "(.h/.hpp/.c/.cc/.cpp/.cs/.ini/.txt + Build.cs) — binary .uasset/.umap " +
    "assets are NEVER read or listed (ADR-006 offline scope; use the live " +
    "asset_find when the bridge is up). The listing never escapes the project " +
    "root (a symlink/junction pointing outside is skipped). SCOPE (ADR-006): " +
    "project files only — no .uasset offline parse. Route: offline (always). " +
    "Error codes: invalid_parameter (malformed body OR a `list` root outside " +
    "the allow-list). A missing .uproject is NOT an error — it returns " +
    "uproject.found:false on a non-error envelope.",
  inputSchema: {
    type: "object",
    properties: {
      list: {
        enum: ["Source", "Config", "Content"],
        description:
          "Optional allow-listed project root to list files under. 'Source' " +
          "(.h/.hpp/.c/.cc/.cpp/.cs + Build.cs), 'Config' (.ini/.txt), or " +
          "'Content' (text extensions only — binary .uasset/.umap are NEVER " +
          "read, ADR-006). Omit to return the .uproject descriptor alone.",
      },
      recursive: {
        type: "boolean",
        default: true,
        description:
          "Recurse sub-folders in the listing. Default true. Ignored when " +
          "`list` is omitted.",
      },
      max_files: {
        type: "integer",
        default: 200,
        minimum: 1,
        description:
          "Hard cap on the number of files a listing returns (default 200). " +
          "When more files exist, `truncated` is true. Ignored when `list` is " +
          "omitted.",
      },
    },
    additionalProperties: false,
  },
};
