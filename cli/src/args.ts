// Thin CLI argument parsing — no runtime deps.
//
// Adapted from Unity Open MCP's mcp-server/src/cli/args.ts. The parser is
// hand-rolled and intentionally small: it covers the P8 MVP command surface and
// their shared global options. Anything more complex should live in a future
// command module.
//
// P8.1 scope: scaffold only. The commands are RECOGNIZED (so --help lists them
// and unknown tokens are rejected with a helpful message), but no command has a
// real handler yet — the dispatcher reports them as "not implemented" until
// P8.2–P8.5 land their modules.
//
// Command shapes (planned):
//   unreal-open-mcp-cli install-plugin [...]
//   unreal-open-mcp-cli setup-mcp [...]
//   unreal-open-mcp-cli open [...]
//   unreal-open-mcp-cli wait-for-ready [...]
//   unreal-open-mcp-cli status [...]
//   unreal-open-mcp-cli configure [...]
//   unreal-open-mcp-cli --help | -h
//   unreal-open-mcp-cli --version | -V
//
// Shared global options (parsed here, consumed by future commands):
//   --project <path> | -P <path>   override UNREAL_PROJECT_PATH
//   --port <n>      | -p <n>       override UNREAL_OPEN_MCP_BRIDGE_PORT
//   --json                        emit JSON instead of human-readable output
//   -v, --verbose                 verbose diagnostics (reserved; honored later)
//
// Per-command options (parsed here so the global parser can keep rejecting
// genuinely unknown tokens; consumed by the matching command module):
//   install-plugin:
//     --plugin-source <dir>   monorepo root to source the plugins from
//     --symlink               dev-mode symlink install (default: copy)
//     --with-verify           install the verify plugin too (default)
//     --no-verify             skip the verify plugin
//     --dry-run               resolve + report, write nothing
//
//   setup-mcp:
//     <agent>                 agent id (cursor, claude-desktop, ...) — use --list
//     --list                  list supported agent ids and config paths
//     --server-command <cmd>  override the MCP server command (default: npx)
//     --dry-run               print the snippet instead of writing it
//
//   open:
//     <projectDir>            project dir (also accepted positionally)
//     --engine-root <dir>     explicit engine install root (source builds)
//     --no-build              accepted for forward compat (MVP does not pre-build)
//
//   wait-for-ready:
//     <projectDir>            project dir (also accepted positionally)
//     --timeout <ms>          overall wait budget (default 120000)
//     --interval <ms>         sleep between polls (default 2000)
//
//   status:
//     <projectDir>            project dir (also accepted positionally)
//     --no-probe              skip the live /ping probe
//
//   configure:
//     <projectDir>            project dir (also accepted positionally)
//     --bridge-port <n>       set the bridge port override in settings
//     --clear-bridge-port     clear the bridge port override
//     --dry-run               resolve + report, write nothing

export type CliCommand =
  | "install-plugin"
  | "setup-mcp"
  | "open"
  | "wait-for-ready"
  | "status"
  | "configure"
  | "help"
  | "version";

/**
 * Commands with real handlers today. Everything in KNOWN_COMMANDS is parsed
 * and listed in --help, but only these have a dispatcher branch wired up. The
 * remainder surface a structured "not implemented yet" message so an agent or
 * script gets a clean signal instead of a silent no-op.
 *
 * P8.1: NONE are implemented. P8.2 appends `install-plugin` as its module
 * lands; P8.3+ appends each later command.
 */
export const IMPLEMENTED_COMMANDS: readonly string[] = [
  "install-plugin",
  "setup-mcp",
  "open",
  "wait-for-ready",
  "status",
  "configure",
];

/** Every command the parser recognizes (and --help advertises). */
export const KNOWN_COMMANDS: readonly string[] = [
  "install-plugin",
  "setup-mcp",
  "open",
  "wait-for-ready",
  "status",
  "configure",
];

export interface ParsedCli {
  command: CliCommand | null;
  /** Bare `--json` flag — switches human-readable output to JSON. */
  json: boolean;
  /** `-v` / `--verbose` — reserved, honored by later commands. */
  verbose: boolean;
  /** Resolved project path override (flag wins; else UNREAL_PROJECT_PATH env at call time). */
  projectPath: string | undefined;
  /** Resolved bridge port override (flag wins; else UNREAL_OPEN_MCP_BRIDGE_PORT env at call time). */
  port: number | undefined;
  /**
   * install-plugin: `--plugin-source <dir>` override (a monorepo root).
   * Undefined when not passed.
   */
  pluginSource: string | undefined;
  /**
   * install-plugin: `--symlink` (dev-mode symlink install). False when not
   * passed; never `undefined` so the command module can read it as a boolean.
   */
  symlink: boolean;
  /**
   * install-plugin: tri-state of `--with-verify` / `--no-verify`.
   * `undefined` = not passed (command applies its own default of `true`);
   * `true` / `false` = explicit.
   */
  withVerify: boolean | undefined;
  /** install-plugin: `--dry-run` — resolve + report, write nothing. */
  dryRun: boolean;
  /**
   * setup-mcp: `--list` — list supported agent ids + config paths and exit.
   * Consumed only when the command is setup-mcp.
   */
  list: boolean;
  /**
   * setup-mcp: `--server-command <cmd>` — override the MCP server command
   * (default `npx`, args derived from the resolved server package). Consumed
   * only when the command is setup-mcp.
   */
  serverCommand: string | undefined;
  /**
   * open: `--engine-root <dir>` — explicit engine install root (source-build
   * escape hatch). Consumed only when the command is open.
   */
  engineRoot: string | undefined;
  /**
   * open: `--no-build` — accepted for forward compat (MVP does not pre-build
   * via UBT before launch). Consumed only when the command is open.
   */
  noBuild: boolean;
  /**
   * wait-for-ready: `--timeout <ms>` — overall wait budget. Consumed only when
   * the command is wait-for-ready.
   */
  timeout: number | undefined;
  /**
   * wait-for-ready: `--interval <ms>` — sleep between polls. Consumed only when
   * the command is wait-for-ready.
   */
  interval: number | undefined;
  /**
   * status: `--no-probe` — skip the live /ping probe and derive the status from
   * the instance lock alone. Consumed only when the command is status.
   */
  noProbe: boolean;
  /**
   * configure: `--bridge-port <n>` — set the bridge port override in the
   * project's settings file. Consumed only when the command is configure.
   */
  bridgePort: number | undefined;
  /**
   * configure: `--clear-bridge-port` — clear (delete) the bridge port override.
   * Consumed only when the command is configure.
   */
  clearBridgePort: boolean;
  /** Leftover positionals after the command token (forwarded to future command modules). */
  positionals: string[];
  /** Parse error message; when set, the dispatcher prints it and exits non-zero. */
  error: string | undefined;
  /** Unknown / unparsed flag tokens (currently an error condition). */
  unknown: string[];
}

export function emptyParsed(): ParsedCli {
  return {
    command: null,
    json: false,
    verbose: false,
    projectPath: undefined,
    port: undefined,
    pluginSource: undefined,
    symlink: false,
    withVerify: undefined,
    dryRun: false,
    list: false,
    serverCommand: undefined,
    engineRoot: undefined,
    noBuild: false,
    timeout: undefined,
    interval: undefined,
    noProbe: false,
    bridgePort: undefined,
    clearBridgePort: false,
    positionals: [],
    error: undefined,
    unknown: [],
  };
}

/**
 * Parse the CLI argv (everything after `node dist/index.js`). Returns a
 * structured result; the dispatcher interprets `command`/`error`. Never throws
 * — parse problems are reported through `error`.
 */
export function parseCliArgs(argv: string[]): ParsedCli {
  const parsed = emptyParsed();
  // Mutable copy; walked with an index so value-taking flags consume their token.
  const args = argv.slice();

  let i = 0;
  let sawCommand = false;

  while (i < args.length) {
    const tok = args[i];

    // --- flags that take no value ---
    if (tok === "--json") {
      parsed.json = true;
      i++;
      continue;
    }
    if (tok === "-v" || tok === "--verbose") {
      parsed.verbose = true;
      i++;
      continue;
    }
    if (tok === "-h" || tok === "--help") {
      parsed.command = "help";
      return parsed;
    }
    if (tok === "-V" || tok === "--version") {
      parsed.command = "version";
      return parsed;
    }

    // --- flags that take a value ---
    if (tok === "--project" || tok === "-P") {
      const v = args[i + 1];
      if (!v || v.startsWith("-")) {
        parsed.error = `${tok} requires a project path.`;
        return parsed;
      }
      parsed.projectPath = v;
      i += 2;
      continue;
    }
    if (tok === "--port" || tok === "-p") {
      const v = args[i + 1];
      const n = parsePort(v);
      if (n === undefined) {
        parsed.error = `${tok} requires a valid port number (1-65535).`;
        return parsed;
      }
      parsed.port = n;
      i += 2;
      continue;
    }

    // --- install-plugin options (parsed globally so unknown tokens are still
    //     rejected; consumed only when the command is install-plugin). ---
    if (tok === "--plugin-source") {
      const v = args[i + 1];
      if (!v || v.startsWith("-")) {
        parsed.error = `${tok} requires a directory path.`;
        return parsed;
      }
      parsed.pluginSource = v;
      i += 2;
      continue;
    }
    if (tok === "--symlink") {
      parsed.symlink = true;
      i++;
      continue;
    }
    if (tok === "--with-verify") {
      parsed.withVerify = true;
      i++;
      continue;
    }
    if (tok === "--no-verify") {
      parsed.withVerify = false;
      i++;
      continue;
    }
    if (tok === "--dry-run") {
      parsed.dryRun = true;
      i++;
      continue;
    }

    // --- setup-mcp options (parsed globally so unknown tokens are still
    //     rejected; consumed only when the command is setup-mcp). ---
    if (tok === "--list") {
      parsed.list = true;
      i++;
      continue;
    }
    if (tok === "--server-command") {
      const v = args[i + 1];
      if (!v || v.startsWith("-")) {
        parsed.error = `${tok} requires a command string.`;
        return parsed;
      }
      parsed.serverCommand = v;
      i += 2;
      continue;
    }

    // --- open options (parsed globally so unknown tokens are still rejected;
    //     consumed only when the command is open). ---
    if (tok === "--engine-root") {
      const v = args[i + 1];
      if (!v || v.startsWith("-")) {
        parsed.error = `${tok} requires a directory path.`;
        return parsed;
      }
      parsed.engineRoot = v;
      i += 2;
      continue;
    }
    if (tok === "--no-build") {
      parsed.noBuild = true;
      i++;
      continue;
    }

    // --- wait-for-ready options (parsed globally so unknown tokens are still
    //     rejected; consumed only when the command is wait-for-ready). ---
    if (tok === "--timeout") {
      const v = args[i + 1];
      const n = parsePositiveInt(v);
      if (n === undefined) {
        parsed.error = `${tok} requires a positive integer (milliseconds).`;
        return parsed;
      }
      parsed.timeout = n;
      i += 2;
      continue;
    }
    if (tok === "--interval") {
      const v = args[i + 1];
      const n = parsePositiveInt(v);
      if (n === undefined) {
        parsed.error = `${tok} requires a positive integer (milliseconds).`;
        return parsed;
      }
      parsed.interval = n;
      i += 2;
      continue;
    }

    // --- status options (parsed globally so unknown tokens are still rejected;
    // consumed only when the command is status). ---
    if (tok === "--no-probe") {
      parsed.noProbe = true;
      i++;
      continue;
    }

    // --- configure options (parsed globally so unknown tokens are still
    // rejected; consumed only when the command is configure). ---
    if (tok === "--bridge-port") {
      const v = args[i + 1];
      const n = parsePort(v);
      if (n === undefined) {
        parsed.error = `${tok} requires a valid port number (1-65535).`;
        return parsed;
      }
      parsed.bridgePort = n;
      i += 2;
      continue;
    }
    if (tok === "--clear-bridge-port") {
      parsed.clearBridgePort = true;
      i++;
      continue;
    }

    // --- positionals ---
    if (!tok.startsWith("-")) {
      if (!sawCommand) {
        if (!KNOWN_COMMANDS.includes(tok)) {
          parsed.error = `Unknown command '${tok}'. Known: ${KNOWN_COMMANDS.join(", ")}.`;
          return parsed;
        }
        // narrowed by the includes() check above
        parsed.command = tok as CliCommand;
        sawCommand = true;
      } else {
        parsed.positionals.push(tok);
      }
      i++;
      continue;
    }

    // Anything else is an unknown flag. Collect for a helpful error.
    parsed.unknown.push(tok);
    i++;
  }

  if (parsed.unknown.length > 0) {
    parsed.error = `Unknown option(s): ${parsed.unknown.join(", ")}.`;
    return parsed;
  }

  return parsed;
}

/** Parse a TCP port: positive integer in [1, 65535]. Rejects anything else. */
export function parsePort(v: string | undefined): number | undefined {
  if (v === undefined || v.startsWith("-")) return undefined;
  const n = Number(v);
  if (!Number.isInteger(n) || n < 1 || n > 65535) return undefined;
  return n;
}

/**
 * Parse a positive integer (milliseconds) for `--timeout` / `--interval`.
 * Rejects anything that is not a positive integer (a leading `-` would
 * otherwise parse as a flag value). Rejects 0 — a zero budget / interval is not
 * meaningful for a poll loop.
 */
export function parsePositiveInt(v: string | undefined): number | undefined {
  if (v === undefined || v.startsWith("-")) return undefined;
  const n = Number(v);
  if (!Number.isInteger(n) || n <= 0) return undefined;
  return n;
}
