# unreal-open-mcp-cli

Setup and ops command-line tool for **Unreal Open MCP** — plugin install, MCP
client wiring, editor launch, and bridge health. Wraps the stdio MCP server for
scripting and CI.

This is a separate package from the MCP server: the stdio server ships under the
`unreal-open-mcp` bin, while this `unreal-open-mcp-cli` bin covers setup and ops.

## Status

The commands below are **recognized** (so `--help` lists them and unknown tokens
are rejected with a helpful message); command handlers land incrementally:

| Command | Status |
|---|---|
| `install-plugin` | **implemented** |
| `setup-mcp` | planned |
| `open` | planned |
| `wait-for-ready` | planned |
| `status` | planned |
| `configure` | planned |

A recognized-but-unimplemented command exits `2` with a clear "not implemented
yet" message — never a silent no-op.

## install-plugin

Copies the bridge (+ verify) plugin into a project's `Plugins/` folder and
enables them in the `.uproject`, idempotently. No editor launch, no network.

```sh
unreal-open-mcp-cli install-plugin [projectDir]
  [--plugin-source <dir>]   # monorepo root (default: auto-detect)
  [--symlink]               # dev-mode symlink install (default: copy)
  [--with-verify]           # also install the verify plugin (default)
  [--no-verify]             # bridge only
  [--dry-run]               # resolve + report, write nothing
```

`projectDir` resolves in this order: the positional arg, `--project <path>`,
`$UNREAL_PROJECT_PATH`, then the current working directory.

The plugin source resolves in this order: `--plugin-source <dir>`,
`$UNREAL_OPEN_MCP_ROOT`, then a walk up from the CLI's own location looking for
a `packages/bridge/UnrealOpenMCP.uplugin` (a local monorepo checkout).

Behaviour notes:

- Copy mode excludes `Intermediate/` and `Binaries/` (UBT build artifacts) so a
  copy from a dev checkout never ships stale compiled modules.
- Symlink mode creates a directory symlink to the source tree, for the
  edit-compile loop. On Windows, creating a symlink needs Developer Mode or
  administrator rights; otherwise the command fails with a clear error.
- The `.uproject` enable is an idempotent upsert — a re-run on an already
  enabled plugin leaves the descriptor byte-identical and is a no-op success.
- `--with-verify` is the default because the bridge plugin's `.uplugin`
  declares the verify plugin as an enabled dependency.

With `--json`, success emits the result envelope on stdout; failure emits it on
stderr (exit `2`).

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
| `UNREAL_OPEN_MCP_ROOT` | Monorepo root the `install-plugin` command sources the plugins from (when `--plugin-source` is not passed). |

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
