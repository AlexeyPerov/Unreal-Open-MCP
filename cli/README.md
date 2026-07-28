# unreal-open-mcp-cli

Setup and ops command-line tool for **Unreal Open MCP** — plugin install, MCP
client wiring, editor launch, and bridge health. Wraps the stdio MCP server for
scripting and CI.

This is a separate package from the MCP server: the stdio server ships under the
`unreal-open-mcp` bin, while this `unreal-open-mcp-cli` bin covers setup and ops.

## Status

Scaffold only. The commands below are **recognized** (so `--help` lists them and
unknown tokens are rejected with a helpful message), but command handlers land
incrementally:

| Command | Status |
|---|---|
| `install-plugin` | planned |
| `setup-mcp` | planned |
| `open` | planned |
| `wait-for-ready` | planned |
| `status` | planned |
| `configure` | planned |

A recognized-but-unimplemented command exits `2` with a clear "not implemented
yet" message — never a silent no-op.

## Usage

```sh
unreal-open-mcp-cli --help
unreal-open-mcp-cli --version
```

### Global options

| Option | Description |
|---|---|
| `--project <path>`, `-P <path>` | Unreal project path (default: `UNREAL_PROJECT_PATH`). |
| `--port <n>`, `-p <n>` | Bridge port override (default: `UNREAL_OPEN_MCP_BRIDGE_PORT`). |
| `--json` | Emit JSON instead of human-readable output (where supported). |
| `-v`, `--verbose` | Verbose diagnostics (reserved; honored by later commands). |

### Environment

| Variable | Purpose |
|---|---|
| `UNREAL_PROJECT_PATH` | Absolute project root (the `.uproject` parent). |
| `UNREAL_OPEN_MCP_BRIDGE_PORT` | Optional bridge port override. |

## Exit codes

| Code | Meaning |
|---|---|
| `0` | success — command completed; no issues. |
| `1` | warnings — non-fatal warnings (e.g. partial setup). |
| `2` | errors — parse error, failed command, invalid arguments, or a recognized-but-unimplemented command. |
| `3` | timeout — the bridge never became reachable, or a call timed out. |

## Develop

```sh
cd cli
npm install
npm run typecheck
npm test
npm run build
node dist/index.js --help
node dist/index.js --version
```

The CLI has **no runtime dependencies** — the argv parser is hand-rolled and
intentionally small. Do not add a dependency without strong justification.
