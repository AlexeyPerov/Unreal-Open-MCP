// Light `helpText` + `versionText` formatters.
//
// Adapted from Unity Open MCP's mcp-server/src/cli/help-text.ts. These are the
// only formatters needed on the `--help` / `--version` fast paths, which must
// NOT pull in any heavy module graph. Keeping them in their own module lets
// cli.ts import only this light file for the fast paths and defer any heavier
// imports to a dynamic `import()` that runs only when a real (future) command
// is dispatched.

import {
  BIN_NAME,
  PORT_ENV_VAR,
  PROJECT_PATH_ENV_VAR,
} from "./constants.js";

export function helpText(binName: string = BIN_NAME): string {
  return [
    `Usage: ${binName} <command> [options]`,
    "",
    "Setup and ops CLI for Unreal Open MCP — plugin install, MCP client wiring,",
    "editor launch, and bridge health. Wraps the stdio MCP server for scripting/CI.",
    "",
    "Commands:",
    "  install-plugin                Copy the bridge (+ verify) plugin into a project and enable it.",
    "  setup-mcp                     Write stdio MCP client configs (Cursor, Claude, ...).",
    "  open                          Launch the Unreal Editor for a project with the bridge loaded.",
    "  wait-for-ready                Poll the bridge until it is reachable; exit 0/non-zero.",
    "  status                        Show resolved bridge port, instance lock, and readiness.",
    "  configure                     Read/write local Unreal Open MCP settings.",
    "  --help, -h                    Show this help.",
    "  --version, -V                 Print the CLI version.",
    "",
    "Exit codes:",
    "  0  success        command completed; no issues.",
    "  1  warnings       non-fatal warnings (e.g. partial setup).",
    "  2  errors         a parse error, a failed command, or invalid arguments.",
    "  3  timeout        the bridge never became reachable, or a call timed out.",
    "",
    "Options:",
    "  --json                        Emit JSON instead of human-readable output (where supported).",
    `  --project <path>, -P <path>   Unreal project path (default: ${PROJECT_PATH_ENV_VAR}).`,
    `  --port <n>, -p <n>            Bridge port override (default: ${PORT_ENV_VAR}).`,
    "  -v, --verbose                 Verbose diagnostics (reserved; honored by later commands).",
    "",
    "Environment:",
    `  ${PROJECT_PATH_ENV_VAR.padEnd(34)}Absolute project root (the .uproject parent).`,
    `  ${PORT_ENV_VAR.padEnd(34)}Optional bridge port override.`,
    "",
    "Examples:",
    `  ${binName} install-plugin --project /path/to/MyProject`,
    `  ${binName} setup-mcp cursor --project /path/to/MyProject`,
    `  ${binName} open --project /path/to/MyProject`,
    `  ${binName} wait-for-ready`,
    `  ${binName} status --json`,
  ].join("\n");
}

export function versionText(version: string, binName: string = BIN_NAME): string {
  return `${binName} ${version}`;
}
