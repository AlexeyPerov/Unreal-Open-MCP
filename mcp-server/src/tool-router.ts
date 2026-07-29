// ToolRouter — the single dispatch spine for every MCP tool call.
//
// P8.6 scope: replace the ad-hoc `LOCAL_TOOLS` + live-router branching that
// lived inline in `index.ts` with one policy table that classifies a tool name
// into a route — `live` | `offline` | `local` | `batch` — and stamps the
// chosen route as metadata on the JSON result so an agent (or the integration
// suite) can branch on where a response came from.
//
// Today three routes are wired end-to-end:
//   - **local** — `unreal_open_mcp_capabilities` + `unreal_open_mcp_bridge_status`
//     resolve entirely in-process (no bridge round-trip). capabilities builds
//     the surface from the in-memory tool list + static rule/fix catalog;
//     bridge_status composes the instance-lock classifier with one /ping probe
//     fired through the live transport (a dead/stopped bridge is a successful
//     status read, not an error).
//   - **offline** (P8.7) — `unreal_open_mcp_read_compile_errors`,
//     `unreal_open_mcp_source_read_offline`, and `unreal_open_mcp_project_index`
//     resolve from disk with the editor DOWN (no bridge hop). read_compile_errors
//     is the one channel that survives a dead bridge assembly — it reads the
//     editor log tail the editor writes independently of the bridge; the source
//     + project readers give an agent enough to orient when the bridge is dead.
//   - **live** — the default. Every tool that is not local/offline/batch is
//     POSTed to the bridge via the injected `LiveClient`
//     (`unreal_open_mcp_ping` → GET /ping; everything else → POST /tools/{name}).
//
// The remaining route is recognized but refuses until its handler lands:
//   - **batch** — empty today. Headless commandlet dispatch is owned by a later
//     phase; a tool routed batch returns `batch_not_implemented` pointing at
//     future commandlet support.
//
// Adapted from Unity Open MCP's mcp-server/src/tool-router.ts (copy fidelity
// for the skeleton + route-metadata contract). Intentional deltas documented
// at the bottom of this file.

import type { CallToolResult } from "@modelcontextprotocol/sdk/types.js";
import type { Router } from "./live-client.js";
import { ALL_TOOLS } from "./tools/index.js";
import { buildCapabilities } from "./capabilities/build-capabilities.js";
import { RULE_CATALOG, FIX_CATALOG } from "./capabilities/rule-catalog.js";
import {
  readInstanceLock,
  classifyInstance,
  lockPath,
  type InstanceLock,
} from "./instance-discovery.js";
import {
  deriveBridgeStatus,
  summarizeInstanceLock,
  bridgeStatusRecoveryHint,
  bridgeStatusNextStep,
  type PingProbe,
} from "./tools/bridge-status-helpers.js";
// P8.7 — offline disk readers. Read-only filesystem parsers resolved from disk
// (no editor needed). No new runtime deps beyond node built-ins.
import {
  resolveEditorLogPath,
  readLogTail,
  DEFAULT_LOG_TAIL_BYTES,
} from "./offline/editor-log.js";
import {
  summarizeProjectHealth,
  compileErrorStatus,
} from "./offline/compile-errors.js";
import { readSourceOffline } from "./offline/source-read.js";
import { buildProjectIndex } from "./offline/project-index.js";

export type { CallToolResult, Router };

/**
 * The four route classes. Every tool name resolves to exactly one via
 * {@link routePolicy}. `live` is the default for any name not in the
 * offline/local/batch sets.
 */
export type Route = "live" | "offline" | "local" | "batch";

/**
 * Route metadata stamped onto every JSON tool result so callers can branch on
 * where a response originated without scraping prose. Mirrors Unity's `_route`
 * shape; the flat `route` token is the load-bearing field, `fallbackReason`
 * is reserved for the future live→batch fallback (unused today).
 */
export interface RouteMeta {
  route: Route;
  fallbackReason?: string;
}

/**
 * `_source` is the coarse transport tag (where the bytes came from). It is
 * kept distinct from {@link RouteMeta.route} because a single route can fetch
 * from more than one transport in the future (e.g. offline-first tools that
 * fall back to live). Today `live`/`local` map 1:1 to the route.
 */
export type SourceTag = "live" | "offline" | "local";

// ---------------------------------------------------------------------------
// Policy table — the single source of truth for route classification.
//
// Adding a new routed tool means extending the matching set here (and wiring
// its handler below). The router itself never branches on a tool name except
// through these sets + the local-handler map. Unity keeps a similar table;
// the Unreal port starts narrow (local only) and widens as offline (P8.7) and
// batch land.
// ---------------------------------------------------------------------------

/**
 * Local-route tools — resolved in-process, never hit the bridge. Adding a
 * local-route tool means extending this set AND adding a handler in
 * {@link ToolRouter.routeLocal}. Kept in sync with the `LOCAL_TOOLS` set in
 * `capabilities/build-capabilities.ts` (the capability surface reports the
 * same route per tool).
 */
const LOCAL_ROUTE_TOOLS: ReadonlySet<string> = new Set([
  "unreal_open_mcp_capabilities",
  "unreal_open_mcp_bridge_status",
]);

/**
 * Offline-route tools — local disk parsers, no editor needed. P8.7 populates
 * this with the three offline read surfaces: `read_compile_errors` (editor log
 * tail → structured diagnostics — the one channel that works when the bridge
 * module itself failed to compile), `source_read_offline` (offline twin of
 * source_read with the same Source/ jail), and `project_index` (.uproject
 * basics + optional Source/Config/Content file listing). Kept in sync with the
 * `OFFLINE_TOOLS` set in `capabilities/build-capabilities.ts`. A name here
 * without a matching handler in {@link ToolRouter.routeOffline} yields
 * `offline_not_implemented` (never a silent live fallthrough).
 */
const OFFLINE_ROUTE_TOOLS: ReadonlySet<string> = new Set([
  "unreal_open_mcp_read_compile_errors",
  "unreal_open_mcp_source_read_offline",
  "unreal_open_mcp_project_index",
]);

/**
 * Batch-route tools — headless commandlet dispatch. Empty today; a later phase
 * owns the commandlet spawn. A name here yields `batch_not_implemented`.
 */
const BATCH_ROUTE_TOOLS: ReadonlySet<string> = new Set([]);

/**
 * Resolve the route for a tool name. The default is `live` — every tool that
 * is not explicitly offline/local/batch is POSTed to the bridge. Exported so
 * the capability surface (and tests) can assert the classification without
 * booting the router.
 */
export function routePolicy(name: string): Route {
  if (LOCAL_ROUTE_TOOLS.has(name)) return "local";
  if (OFFLINE_ROUTE_TOOLS.has(name)) return "offline";
  if (BATCH_ROUTE_TOOLS.has(name)) return "batch";
  return "live";
}

// ---------------------------------------------------------------------------
// Route-metadata helpers. Every route-tagging site goes through these so
// live / offline / local stay symmetric — a new route adding `_source` / `_route`
// inline is caught by the "no inline route stamps outside the helpers" rule.
// ---------------------------------------------------------------------------

/**
 * Stamp `_source` + `_route` onto a body object literal. The common shape for
 * local/offline routes that synthesize the entire response in the MCP server
 * (no bridge tool endpoint).
 */
function withRouteTags<T extends Record<string, unknown>>(
  body: T,
  source: SourceTag,
  route: Route,
): T & { _source: SourceTag; _route: RouteMeta } {
  return { ...body, _source: source, _route: { route } };
}

/**
 * Build a single-block CallToolResult from a body object, tagged with
 * `_source` + `_route`. Accepts the typed result shapes (CapabilitiesResult,
 * BridgeStatusBody, …) returned by the local builders.
 */
function sourceResult(
  body: object,
  source: SourceTag,
  route: Route,
  isError = false,
): CallToolResult {
  const tagged = withRouteTags(
    body as Record<string, unknown>,
    source,
    route,
  );
  return {
    content: [{ type: "text", text: JSON.stringify(tagged) }],
    isError,
  };
}

/**
 * Stamp `_source` + `_route` onto an already-built CallToolResult's first JSON
 * text block. Used by the live path (the bridge built the body; the router
 * only adds the route metadata). A no-op when the first text block is not a
 * parseable JSON object (e.g. an MCP image content block) — preserves
 * `isError` and any other content blocks.
 */
function tagResult(
  result: CallToolResult,
  source: SourceTag,
  meta: RouteMeta,
): CallToolResult {
  if (result.content.length === 0) return result;
  // Screenshot tools return an MCP image content block followed by a text
  // metadata block. Inject the tags into whichever text block carries JSON;
  // leave image/other blocks untouched. Fall back to the first block when it
  // is text (the common single-block path).
  const textIndex = result.content.findIndex((c) => c.type === "text");
  if (textIndex < 0) return result;
  const block = result.content[textIndex];
  if (block.type !== "text") return result;
  try {
    const body = JSON.parse(block.text) as Record<string, unknown>;
    const newContent = result.content.slice();
    newContent[textIndex] = {
      type: "text",
      text: JSON.stringify({ ...body, _source: source, _route: meta }),
    };
    return { ...result, content: newContent };
  } catch {
    return result;
  }
}

/**
 * Build a local-routed error result (missing-parameter refusals, etc.). Tagged
 * `_source: "local"` + `_route: { route: "local" }` because it is resolved
 * entirely in the MCP server.
 */
function localError(code: string, message: string): CallToolResult {
  return sourceResult({ error: { code, message } }, "local", "local", true);
}

/**
 * Build an offline-routed error result (a jail escape, a read failure, etc.).
 * Tagged `_source: "offline"` + `_route: { route: "offline" }` because it is
 * resolved from disk (the disk parse failed, but the response still came from
 * the offline path — agents branch on `_source`/`_route`).
 */
function offlineError(code: string, message: string): CallToolResult {
  return sourceResult({ error: { code, message } }, "offline", "offline", true);
}

/**
 * Map a `source_read_offline` error code to a human-readable message (mirrors
 * the live source_read error messages so an agent treats both paths alike).
 */
function describeSourceReadError(
  code: "path_escapes_jail" | "file_not_found" | "not_a_file" | "read_failed",
  path: string,
): string {
  switch (code) {
    case "path_escapes_jail":
      return (
        `Path '${path}' escapes the <Project>/Source/ jail. Only relative-to-` +
        "Source or absolute-inside-Source paths are accepted; '..' traversal, " +
        "absolute-outside, and NTFS alternate-data-stream (':') escapes are " +
        "refused."
      );
    case "file_not_found":
      return `Source file not found: '${path}' (jailed to <Project>/Source/).`;
    case "not_a_file":
      return `Path '${path}' is a directory, not a file (jailed to <Project>/Source/).`;
    case "read_failed":
      return `Could not read source file '${path}' (jailed to <Project>/Source/).`;
  }
}

// ---------------------------------------------------------------------------
// ToolRouter
// ---------------------------------------------------------------------------

/**
 * The dispatch spine. Holds the live transport (a {@link Router} — today the
 * `LiveClient`) and resolves each tool call through {@link routePolicy} into
 * the matching handler. Constructed once by the MCP server's `main()` after
 * env/port/token resolution; the `tools/call` handler delegates every call to
 * {@link route}.
 *
 * `projectPath` is threaded in so the local `bridge_status` handler can read
 * this project's instance lock without re-resolving env at call time. Optional
 * callers (tests that drive a single handler directly) may leave it null —
 * bridge_status then reports a lock-less status (classification `gone`).
 */
export class ToolRouter implements Router {
  constructor(
    private readonly live: Router,
    private readonly projectPath: string | null = null,
  ) {}

  /**
   * Dispatch a tool call. The single entry point for `tools/call`. Classifies
   * the name via {@link routePolicy} and delegates to the matching handler;
   * every successful AND error response carries `_source` + `_route` metadata.
   */
  async route(
    toolName: string,
    args: Record<string, unknown>,
  ): Promise<CallToolResult> {
    const policy = routePolicy(toolName);
    switch (policy) {
      case "local":
        return this.routeLocal(toolName, args);
      case "offline":
        return this.routeOffline(toolName, args);
      case "batch":
        // A later phase owns the headless commandlet spawn. Recognize the
        // policy so a future batch tool is never accidentally POSTed live,
        // but refuse until the spawn lands.
        return localError(
          "batch_not_implemented",
          `Tool ${toolName} is classified batch-route (headless commandlet). ` +
            `Commandlet dispatch is not implemented yet; use the live bridge ` +
            `equivalent or call unreal_open_mcp_capabilities for the current surface.`,
        );
      case "live":
      default: {
        const result = await this.live.route(toolName, args);
        return tagResult(result, "live", { route: "live" });
      }
    }
  }

  /**
   * Resolve a local-route tool in-process. Returns a tagged CallToolResult for
   * every known local tool; an unknown name that nonetheless classified local
   * (a policy-table / handler drift bug) surfaces as a structured local error
   * rather than crashing.
   */
  private async routeLocal(
    name: string,
    args: Record<string, unknown>,
  ): Promise<CallToolResult> {
    if (name === "unreal_open_mcp_capabilities") {
      return this.routeCapabilities(args);
    }
    if (name === "unreal_open_mcp_bridge_status") {
      return this.routeBridgeStatus();
    }
    // Unreachable when the policy table and handler set are in sync. Defensive
    // fallback keeps the call honest — a live fallthrough here would mask the
    // drift.
    return localError(
      "local_handler_missing",
      `Tool ${name} is registered local-route but has no in-process handler. ` +
        `This is a policy-table / handler drift bug; extend LOCAL_ROUTE_TOOLS ` +
        `and routeLocal together.`,
    );
  }

  /**
   * Resolve an offline-route tool from disk. Returns a tagged CallToolResult
   * (stamped `_source: "offline"` + `_route: { route: "offline" }`) for every
   * known offline tool; an unknown name that nonetheless classified offline
   * (a policy-table / handler drift bug) surfaces as a structured offline error
   * rather than crashing. All three offline handlers work with the editor down
   * — they never touch the live transport.
   *
   * A null `projectPath` (no `UNREAL_PROJECT_PATH`) is refused with
   * `project_path_not_bound` — the offline readers need the project root to
   * resolve the log dir / Source jail / `.uproject`. This never falls through
   * to live.
   */
  private async routeOffline(
    name: string,
    args: Record<string, unknown>,
  ): Promise<CallToolResult> {
    const projectPath = this.projectPath;
    if (!projectPath) {
      return offlineError(
        "project_path_not_bound",
        `Tool ${name} is offline-route but the MCP server has no bound project ` +
          `path (set UNREAL_PROJECT_PATH). Offline readers need the project root ` +
          `to resolve the editor log dir / Source jail / .uproject.`,
      );
    }
    if (name === "unreal_open_mcp_read_compile_errors") {
      return this.routeReadCompileErrors(projectPath, args);
    }
    if (name === "unreal_open_mcp_source_read_offline") {
      return this.routeSourceReadOffline(projectPath, args);
    }
    if (name === "unreal_open_mcp_project_index") {
      return this.routeProjectIndex(projectPath, args);
    }
    // Unreachable when the policy table and handler set are in sync. Defensive
    // fallback — a live fallthrough would mask the drift.
    return offlineError(
      "offline_handler_missing",
      `Tool ${name} is registered offline-route but has no disk-parser handler. ` +
        `This is a policy-table / handler drift bug; extend OFFLINE_ROUTE_TOOLS ` +
        `and routeOffline together.`,
    );
  }

  /**
   * read_compile_errors (P8.7) — read the newest editor log tail and extract
   * structured MSVC / clang diagnostics. Works with the bridge DOWN: it reads
   * `<Project>/Saved/Logs/*.log` directly, the one channel that survives a dead
   * bridge assembly. A missing log is a non-error `log_not_found`; an unreadable
   * log is `editor_log_unreadable`. Never returns `isError:true` for an empty /
   * clean log — that is a successful read of zero diagnostics.
   */
  private async routeReadCompileErrors(
    projectPath: string,
    args: Record<string, unknown>,
  ): Promise<CallToolResult> {
    const tailBytes =
      typeof args.tail_bytes === "number" && args.tail_bytes >= 4096
        ? Math.min(Math.floor(args.tail_bytes), 1048576)
        : DEFAULT_LOG_TAIL_BYTES;

    const logPath = resolveEditorLogPath(projectPath);
    if (!logPath) {
      return sourceResult(
        {
          status: "log_not_found",
          unhealthy: false,
          headline: "",
          error_count: 0,
          errors: [],
          logPath: null,
          message: `No .log found under ${projectPath}/Saved/Logs/. The Unreal ` +
            "Editor may not have written errors yet, or it is running with a " +
            "custom -logFile path this tool does not resolve.",
        },
        "offline",
        "offline",
      );
    }

    const tail = readLogTail(logPath, tailBytes);
    if (tail.error) {
      return offlineError(
        "editor_log_unreadable",
        `Could not read the editor log at ${logPath}: ${tail.error}`,
      );
    }

    const health = summarizeProjectHealth(tail.content);
    const status = compileErrorStatus(health, true);
    return sourceResult(
      {
        status,
        unhealthy: health.unhealthy,
        headline: health.headline,
        error_count: health.errors.filter((e) => e.severity === "error").length,
        errors: health.errors,
        logPath,
        tailBytes: tail.bytes,
      },
      "offline",
      "offline",
    );
  }

  /**
   * source_read_offline (P8.7) — offline twin of source_read. Same Source/ jail
   * + result shape; resolved from disk with the bridge down. The jail rejects
   * `..` / absolute-outside / NTFS ADS escapes with `path_escapes_jail`.
   */
  private async routeSourceReadOffline(
    projectPath: string,
    args: Record<string, unknown>,
  ): Promise<CallToolResult> {
    const path = typeof args.path === "string" ? args.path : "";
    if (!path) {
      return offlineError(
        "missing_parameter",
        "Missing required parameter 'path'.",
      );
    }
    const outcome = readSourceOffline(projectPath, path, {
      start_line: typeof args.start_line === "number" ? args.start_line : undefined,
      end_line: typeof args.end_line === "number" ? args.end_line : undefined,
      max_lines: typeof args.max_lines === "number" ? args.max_lines : undefined,
    });
    if (!outcome.ok) {
      return offlineError(outcome.code, describeSourceReadError(outcome.code, path));
    }
    return sourceResult(outcome.result, "offline", "offline");
  }

  /**
   * project_index (P8.7) — `.uproject` basics + an optional file listing under
   * Source/Config/Content. A missing `.uproject` is a non-error
   * `uproject.found:false`; a `list` root outside the allow-list is
   * `invalid_parameter`. Works with the bridge down.
   */
  private async routeProjectIndex(
    projectPath: string,
    args: Record<string, unknown>,
  ): Promise<CallToolResult> {
    const list =
      typeof args.list === "string" ? args.list : undefined;
    if (
      list !== undefined &&
      list !== "Source" &&
      list !== "Config" &&
      list !== "Content"
    ) {
      return offlineError(
        "invalid_parameter",
        `Invalid 'list' root '${list}'. Must be one of Source / Config / Content.`,
      );
    }
    const recursive = args.recursive !== false;
    const maxFiles =
      typeof args.max_files === "number" && args.max_files >= 1
        ? Math.floor(args.max_files)
        : undefined;

    const result = buildProjectIndex(projectPath, {
      list,
      recursive,
      max_files: maxFiles,
    });
    return sourceResult(result, "offline", "offline");
  }

  /** capabilities (P3.8) — local-route, works with the editor down. */
  private async routeCapabilities(
    args: Record<string, unknown>,
  ): Promise<CallToolResult> {
    const kind =
      args.kind === "tools" || args.kind === "rules" || args.kind === "fixes"
        ? (args.kind as "tools" | "rules" | "fixes")
        : undefined;
    const includePlanned = args.include_planned !== false;
    const result = buildCapabilities(
      {
        tools: ALL_TOOLS,
        rules: RULE_CATALOG,
        fixes: FIX_CATALOG,
      },
      { kind, includePlanned },
    );
    return sourceResult(result, "local", "local");
  }

  /**
   * bridge_status (P5.7) — composes the instance-lock classifier with one /ping
   * probe fired through the live transport, then maps the result with the pure
   * `deriveBridgeStatus`. A dead/stopped bridge is a *successful* status read —
   * this never returns `isError:true`.
   */
  private async routeBridgeStatus(): Promise<CallToolResult> {
    const projectPath = this.projectPath;
    const lock: InstanceLock | null = projectPath
      ? readInstanceLock(projectPath)
      : null;
    const classification = classifyInstance(lock);

    // Fire one /ping probe through the live transport. When the transport is
    // offline the probe fails — the status is then derived from the lock alone
    // (stopped unless a stale-heartbeat lock classifies dead_bridge).
    const ping = await this.probeBridge();

    const status = deriveBridgeStatus({ classification, ping });

    const body = {
      status,
      ready: status === "running",
      projectPath: projectPath ?? null,
      classification,
      recoveryHint: bridgeStatusRecoveryHint(status),
      instance: {
        lockPath: projectPath ? lockPath(projectPath) : null,
        classification,
        lock: summarizeInstanceLock(lock),
      },
      ping:
        ping !== null && ping.kind === "ok"
          ? {
              reachable: true,
              connected: ping.connected,
              compiling: ping.compiling ?? null,
              isPlaying: ping.isPlaying ?? null,
              unrealVersion: ping.unrealVersion ?? null,
              bridgeVersion: ping.bridgeVersion ?? null,
              mode: ping.mode ?? null,
            }
          : { reachable: false },
      nextStep: bridgeStatusNextStep(status),
    };

    return sourceResult(body, "local", "local");
  }

  /**
   * Fire one /ping probe through the live transport and parse the body into a
   * {@link PingProbe}. Returns `{ kind: "fail" }` when the probe errored or the
   * body did not parse — the status mapper only needs reachable-vs-fail plus
   * the health fields on success.
   */
  private async probeBridge(): Promise<PingProbe> {
    let result: CallToolResult;
    try {
      result = await this.live.route("unreal_open_mcp_ping", {});
    } catch {
      return { kind: "fail" };
    }
    if (result.isError) return { kind: "fail" };
    const first = result.content[0];
    if (!first || first.type !== "text" || typeof first.text !== "string") {
      return { kind: "fail" };
    }
    let parsed: Record<string, unknown>;
    try {
      parsed = JSON.parse(first.text);
    } catch {
      return { kind: "fail" };
    }
    if (!parsed || typeof parsed !== "object") return { kind: "fail" };
    return {
      kind: "ok",
      connected: parsed.connected === true,
      compiling:
        typeof parsed.compiling === "boolean" ? parsed.compiling : undefined,
      isPlaying:
        typeof parsed.isPlaying === "boolean" ? parsed.isPlaying : undefined,
      unrealVersion:
        typeof parsed.unrealVersion === "string" ? parsed.unrealVersion : null,
      bridgeVersion:
        typeof parsed.bridgeVersion === "string"
          ? parsed.bridgeVersion
          : undefined,
      mode: typeof parsed.mode === "string" ? parsed.mode : undefined,
    };
  }
}

// Intentional deltas from Unity Open MCP (mcp-server/src/tool-router.ts):
//  - Skeleton + offline reads wired: live + local + offline wired end-to-end.
//    Unity's router additionally owns the compressible-router / AssetModelCache
//    offline asset parsers, batch spawn (BatchSpawn), tool-group manage_tools,
//    generate_skill, hub_*, restart_editor / resource_pressure, and SSE event
//    streaming. Those land in later phases here; the router is built to absorb
//    them via the same routePolicy table + per-route handlers.
//  - Narrower offline set (ADR-006 — no .uasset offline pipeline): only three
//    read surfaces today — read_compile_errors (editor log tail), source_read
//    _offline (Source/ jail), project_index (.uproject + file tree). No
//    compressible-router / AssetModelCache; offline `.uasset` reads stay a
//    later-phase decision (batch commandlet). The offline diagnostic parser
//    mirrors the bridge's ParseDiagnostics (MSVC + clang) instead of Unity's
//    CSxxxx extractor.
//  - Narrower batch (stub only). Unity's batch spawn covers compile_check +
//    scan_all + baseline + regression; the Unreal commandlet equivalent is
//    owned by a later phase.
//  - Smaller always-local set: only capabilities + bridge_status today. Unity
//    also routes manage_tools, generate_skill, restart_editor,
//    resource_pressure, and the hub_* family locally. Each lands here as its
//    phase ships.
//  - Route metadata shape mirrors Unity (`_source` + `_route: { route }`); the
//    flat `route` token is the load-bearing field agents branch on. Kept on
//    every successful AND error JSON body so agent recovery logic can trust it.
