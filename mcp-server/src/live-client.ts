// LiveClient — routes MCP tool calls to the live Unreal bridge over loopback HTTP.
//
// P1.7 scope: the `unreal_open_mcp_ping` round-trip only.
//   route("unreal_open_mcp_ping", {}) → handlePing() → GET /ping
//
// Every other tool name returns a structured `tool_not_routed` error. Tool
// dispatch (`POST /tools/{name}`), the compile-wait/503 retry loop, gate
// envelopes, PingCache, and endpoint-refresh all land in later phases — the
// bridge itself does not serve `/tools/*` yet (P2.1). Keeping this surface
// narrow now means a single, honest E2E probe that the P1.9 smoke baseline can
// rely on.
//
// Failure classification (the load-bearing contract):
//   - bridge_offline     → the bridge is not reachable. ECONNREFUSED, DNS, or a
//                          network error that is NOT an abort. The hint names
//                          this project's instance lock path so an agent
//                          debugging a port mismatch knows where to look.
//   - bridge_timeout     → the request connected but did not return within the
//                          timeout (AbortController fired). Distinguished from
//                          bridge_offline so an agent can tell "wrong port /
//                          bridge not running" from "bridge hung mid-request".
//   - bridge_http_error  → the bridge responded with an unexpected HTTP status
//                          (5xx / 4xx other than the documented 503 not-ready).
//                          Carries the bridge's own error body when it sent one.
//
// Adapted from Unity Open MCP's mcp-server/src/live-client.ts (copy fidelity,
// P1.7). Intentional deltas:
//   - `unrealVersion` in PingResponse (not `unityVersion`), plus `status` /
//     `port` fields the Unreal bridge emits.
//   - Minimal class: no PingCache, no compile-wait, no gate envelopes, no
//     endpoint-refresh. The Unity client grew those over many milestones; this
//     port adds them as later phases require.
//   - `tool_not_routed` for non-ping tools is explicit and terminal here (the
//     Unity client falls through to POST /tools/{name}); the Unreal bridge has
//     no tool-dispatch endpoint until P2.1.
//   - Offline hint points at `~/.unreal-open-mcp/instances/...`.

import type { CallToolResult } from "@modelcontextprotocol/sdk/types.js";
import { PORT_OVERRIDE_ENV_VAR, lockPath } from "./instance-discovery.js";
import { makeErrorResult } from "./results.js";

export type { CallToolResult };

/**
 * Live tool router. The MCP server holds one instance per session and dispatches
 * `unreal_open_mcp_*` tool calls through it. Only `unreal_open_mcp_ping` is
 * routed in P1.7; other names return `tool_not_routed`.
 */
export interface Router {
  route(
    toolName: string,
    args: Record<string, unknown>,
  ): Promise<CallToolResult>;
}

/**
 * /ping response body shape emitted by the Unreal bridge
 * (FUnrealOpenMcpBridgeJson::BuildPingJson). Field order is pinned by the bridge
 * spec; here we only assert the field set. `unrealVersion` (not `unityVersion`)
 * and the extra `status` / `port` fields are the Unreal deltas vs the Unity
 * bridge.
 */
export interface PingResponse {
  /** True only when the game thread picked the ping dispatch up (load-bearing). */
  connected: boolean;
  /** "ready" (HTTP 200) or "not_ready" (503 fallback). */
  status: string;
  /** Absolute project directory the bridge bound to. */
  projectPath: string | null;
  /** Engine version string from FEngineVersion::Current(). */
  unrealVersion: string | null;
  /** Bridge/plugin version, mirrored from FUnrealOpenMcpBridgeSession. */
  bridgeVersion: string;
  /** Transport mode label — always "live" for the HTTP bridge. */
  mode: string;
  /** Bound loopback port. */
  port: number;
  /** Stub false in P1.3 — real compile state lands with bridge_status (later). */
  compiling: boolean;
  /** Stub false in P1.3 — real PIE state lands with bridge_status (later). */
  isPlaying: boolean;
}

/**
 * Bridge HTTP error body shape — `{"error":{"code","message"}}`
 * (FUnrealOpenMcpBridgeJson::BuildErrorJson).
 */
interface HttpErrorBody {
  error: {
    code: string;
    message: string;
  };
}

/** Default /ping fetch timeout. Short because /ping is a readiness probe. */
export const PING_TIMEOUT_MS = 5_000;

/** Loopback host the Unreal bridge binds. Mirrors the C++ Loopback constant. */
export const LOOPBACK_HOST = "127.0.0.1";

/** Build the bridge base URL for a given port. Centralizes scheme + host. */
export function bridgeBaseUrl(port: number): string {
  return `http://${LOOPBACK_HOST}:${port}`;
}

/**
 * Build a per-instance offline hint that names THIS project's lock file path so
 * an agent debugging a `bridge_offline` knows exactly where to look. Mirrors
 * Unity's buildOfflineHint but points at the Unreal scratch dir. Falls back to
 * a generic message when the project path is unknown (older callers / tests).
 */
function buildOfflineHint(projectPath: string | undefined): string {
  const base =
    "Ensure the Unreal Editor is open with the Unreal Open MCP bridge running. " +
    "The bridge port is per-project (20000 + sha256(projectPath) % 10000), not " +
    `fixed — if the editor is open the MCP server may be aimed at the wrong port. ` +
    `Check the instance lock at ~/.unreal-open-mcp/instances/<sha256(projectPath)>.json ` +
    `for the live port/pid, or set ${PORT_OVERRIDE_ENV_VAR}. If the editor is not ` +
    `open, launch it with the project loaded.`;
  if (!projectPath) return base;
  return `${base} This project's lock file: ${lockPath(projectPath)}`;
}

/**
 * Is the thrown error an AbortError (fetch timeout via AbortController)?
 * Node's fetch throws a DOMException with name "AbortError" when the abort
 * signal fires. We keep this as a narrow predicate so the bridge_offline vs
 * bridge_timeout split is testable in isolation.
 */
export function isAbortError(err: unknown): boolean {
  if (err == null) return false;
  if (err instanceof DOMException) return err.name === "AbortError";
  const name = (err as { name?: string }).name;
  return name === "AbortError";
}

export class LiveClient implements Router {
  private baseUrl: string;
  /**
   * Per-session bearer token auto-discovered from the instance lock. Undefined
   * when no live lock was found (older bridge / env port override); in that
   * case no Authorization header is sent and the bridge must be in authMode
   * "none" for requests to succeed. The bridge omits the token until a later
   * phase; the wiring here is additive so that phase is a no-op on this side.
   */
  private authToken: string | undefined;
  /**
   * Absolute Unreal project path, threaded into the offline hint so an agent
   * gets the exact lock file path to inspect. Optional so tests that only
   * exercise the ping-success path don't need to plant a lock.
   */
  private projectPath: string | undefined;

  constructor(port: number, authToken?: string, projectPath?: string) {
    this.baseUrl = bridgeBaseUrl(port);
    this.authToken = authToken;
    this.projectPath = projectPath;
  }

  /**
   * Route a tool call. P1.7 routes `unreal_open_mcp_ping` to `GET /ping`; every
   * other name returns a structured `tool_not_routed` error. The error is
   * terminal (no fallback) because the Unreal bridge has no tool-dispatch
   * endpoint yet — that lands in P2.1.
   */
  async route(
    toolName: string,
    _args: Record<string, unknown>,
  ): Promise<CallToolResult> {
    if (toolName === "unreal_open_mcp_ping") {
      return this.handlePing();
    }
    return makeErrorResult({
      code: "tool_not_routed",
      message:
        `Tool '${toolName}' is not routed to the live bridge. The bridge does ` +
        `not serve tool dispatch yet (POST /tools/{name} lands in a later ` +
        `phase); only 'unreal_open_mcp_ping' is live-routed today. Endpoint: ` +
        `${this.baseUrl}.`,
    });
  }

  /**
   * Fetch the bridge health payload. On success the PingResponse body is
   * returned verbatim as a text content block. On failure the error is
   * classified into bridge_offline / bridge_timeout / bridge_http_error so the
   * caller can branch on the cause. A 503 ("not_ready") is surfaced as a
   * structured bridge_http_error carrying the fallback body — the bridge is
   * reachable but the game thread is not responsive, distinct from unreachable.
   */
  private async handlePing(): Promise<CallToolResult> {
    let res: Response;
    try {
      res = await this.fetchWithTimeout("/ping", { method: "GET" }, this.getPingTimeoutMs());
    } catch (err) {
      if (isAbortError(err)) {
        return makeErrorResult({
          code: "bridge_timeout",
          message:
            `Bridge did not respond to /ping within the timeout. The bridge ` +
            `listener at ${this.baseUrl} accepted the connection but did not ` +
            `return — the editor may be blocked (modal dialog, heavy compile) ` +
            `or the game-thread dispatcher is stalled. ${buildOfflineHint(this.projectPath)}`,
        });
      }
      // Connection failure: ECONNREFUSED, DNS, socket reset before connect. The
      // bridge is not reachable on this endpoint.
      return makeErrorResult({
        code: "bridge_offline",
        message:
          `Bridge is not reachable at ${this.baseUrl}. The editor may not be ` +
          `running, or the bridge is on a different port. ${buildOfflineHint(this.projectPath)}`,
      });
    }

    // The bridge returns 503 with a fallback body when the game thread is not
    // responsive (game thread blocked / dispatcher shut down / dispatch timed
    // out). Treat that as "reachable but not ready" — distinct from a hard
    // offline. Parse the body and surface it via bridge_http_error so an agent
    // sees the connected:false / not_ready payload.
    if (!res.ok) {
      const body = (await res.json().catch(() => null)) as HttpErrorBody | PingResponse | null;
      return makeErrorResult({
        code: "bridge_http_error",
        message:
          `Bridge /ping returned HTTP ${res.status}. The listener is up but ` +
          `reported a non-OK status (503 = game thread not ready; other = ` +
          `unexpected bridge error). Endpoint: ${this.baseUrl}.`,
        detail: body ?? {
          error: {
            code: "bridge_http_error",
            message: `HTTP ${res.status}`,
          },
        },
      });
    }

    let body: PingResponse;
    try {
      body = (await res.json()) as PingResponse;
    } catch {
      return makeErrorResult({
        code: "bridge_response_unparsable",
        message:
          `Bridge /ping returned HTTP 200 but the response body was not valid ` +
          `JSON. The bridge may have been torn down mid-response (editor ` +
          `shutdown / reload). Endpoint: ${this.baseUrl}.`,
      });
    }

    return {
      content: [{ type: "text", text: JSON.stringify(body) }],
      isError: false,
    };
  }

  /**
   * Per-ping fetch timeout. Protected so tests can drive a real AbortController
   * abort in milliseconds rather than waiting the full default. Production uses
   * {@link PING_TIMEOUT_MS}.
   */
  protected getPingTimeoutMs(): number {
    return PING_TIMEOUT_MS;
  }

  /**
   * fetch with an AbortController timeout. When `authToken` is set, attaches
   * `Authorization: Bearer <token>` to every request (merged with any caller-
   * supplied header so a per-request value always wins). The token path is
   * inert today (the bridge omits authToken until a later phase); wiring it
   * now keeps that later phase additive.
   */
  private fetchWithTimeout(
    path: string,
    init: RequestInit,
    timeoutMs: number = PING_TIMEOUT_MS,
  ): Promise<Response> {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);

    const headers = new Headers(init.headers);
    if (this.authToken && !headers.has("Authorization")) {
      headers.set("Authorization", `Bearer ${this.authToken}`);
    }

    return fetch(`${this.baseUrl}${path}`, {
      ...init,
      headers,
      signal: controller.signal,
    }).finally(() => clearTimeout(timer));
  }
}
