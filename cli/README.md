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
| `setup-mcp` | **implemented** |
| `open` | **implemented** |
| `wait-for-ready` | **implemented** |
| `status` | **implemented** |
| `configure` | **implemented** |

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

## setup-mcp

Writes a **stdio** MCP client config snippet for a supported AI agent so it can
spawn `unreal-open-mcp` against a project without hand-editing JSON. Stdio only
— no HTTP, no cloud URL, no OAuth. Prefer to edit the config by hand instead?
See [Manual setup](../docs/manual-setup.md) for the same JSON snippets per
client.

```sh
unreal-open-mcp-cli setup-mcp <agent> [projectDir]
  [--project <dir>]        # project root (default: positional / UNREAL_PROJECT_PATH / cwd)
  [--port <n>]             # also write UNREAL_OPEN_MCP_BRIDGE_PORT into the snippet
  [--server-command <cmd>] # override the server command (default: npx -y unreal-open-mcp@<cli-version>)
  [--dry-run]              # print the snippet instead of writing it
  [--list]                 # list supported agent ids and their config paths
```

`<agent>` is one of the supported ids — run `setup-mcp --list` to see them all.
The MVP roster covers Cursor, Claude Desktop, Claude Code, VS Code (Copilot),
Gemini CLI, and Cline.

The snippet always carries the absolute project root under
`UNREAL_PROJECT_PATH`. The server command defaults to
`npx -y unreal-open-mcp@<cli-version>` (the CLI and server version together);
pass `--server-command node` for the monorepo-dev case (launches
`mcp-server/dist/index.js`), or `--server-command unreal-open-mcp` for a global
install.

Behaviour notes:

- A re-run **deep-merges** by server key — sibling MCP servers in the same
  config file are preserved; only the `unreal-open-mcp` entry is replaced.
- A malformed existing config is treated as a warning, not an error: the
  command starts fresh under the agent's body key rather than clobbering the
  user's hand-edited file.
- The optional bridge port is written only when `--port` is passed — the
  command never invents ports.

With `--json`, success emits the result envelope on stdout; failure emits it on
stderr (exit `2`).

## open

Launches the Unreal Editor for a project with the Open MCP bridge loaded. The
editor is spawned detached (fire-and-forget); the command returns immediately
once the process is launched, and `wait-for-ready` polls until the bridge
binds. No cloud, no OAuth, no HTTP URL wiring — the only env propagated onto the
editor is `UNREAL_PROJECT_PATH` (and the bridge port when `--port` is set).

```sh
unreal-open-mcp-cli open [projectDir]
  [--engine-root <dir>]   # explicit engine install root (source builds)
  [--no-build]            # accepted for forward compat (the editor triggers the build itself)
  [--port <n>]            # propagate -UNREAL_OPEN_MCP_BRIDGE_PORT=<n> to the editor launch
```

`projectDir` resolves in this order: the positional arg, `--project <path>`,
`$UNREAL_PROJECT_PATH`, then the current working directory.

Engine resolution is best-effort, in this order:

1. `--engine-root <dir>` — the directory that contains
   `Engine/Binaries/<plat>/UnrealEditor[.exe]`. This is the source-build escape
   hatch and always wins.
2. `$UE_ROOT` — same shape check as `--engine-root`.
3. The `.uproject`'s `EngineAssociation` matched against the Epic Launcher's
   per-OS common install paths (e.g.
   `/Users/<user>/Library/Epic/UE_<assoc>` on macOS,
   `C:\Program Files\Epic Games\UE_<assoc>` on Windows).

When none resolve, the command exits `2` with a clear message pointing at
`--engine-root` / `$UE_ROOT`. A GUID `EngineAssociation` (a source build) can
never be matched by the common-path scan — use `--engine-root` for those.

Behaviour notes:

- The editor is spawned `detached` with `stdio: "ignore"` and `unref`'d, so the
  CLI returns immediately and does not wait for the editor to exit. An async
  spawn failure (e.g. `EACCES`) surfaces as a missing PID and a later
  `wait-for-ready` timeout — the same UX as a slow boot.
- `--port <n>` appends `-UNREAL_OPEN_MCP_BRIDGE_PORT=<n>` to the editor's launch
  args (the bridge reads this P1.4 launch arg) and sets the matching env var.
  When omitted, the bridge binds its deterministic per-project port (see
  `wait-for-ready`).
- `--no-build` is accepted for forward compatibility but the MVP does not invoke
  UnrealBuildTool before launch — the editor triggers the build itself on
  launch when needed.

With `--json`, success emits the result envelope on stdout; failure emits it on
stderr (exit `2`).

## wait-for-ready

Polls the bridge's `GET /ping` until it answers ready (connected, idle) or the
overall timeout elapses. This is the second half of the one-command developer
loop — `open` launches the editor, `wait-for-ready` blocks until the bridge is
usable.

```sh
unreal-open-mcp-cli wait-for-ready [projectDir]
  [--port <n>]        # bridge port override (default: discovered, see below)
  [--timeout <ms>]    # overall wait budget (default 120000)
  [--interval <ms>]   # sleep between polls (default 2000)
```

`projectDir` resolves in this order: the positional arg, `--project <path>`,
`$UNREAL_PROJECT_PATH`, then the current working directory. A relative path is
absolutized against the current working directory before hashing.

The bridge port is resolved with the **same precedence the MCP server uses**, so
the CLI polls the same port the bridge binds:

1. `--port <n>` / `$UNREAL_OPEN_MCP_BRIDGE_PORT` (override wins).
2. The live instance lock at
   `~/.unreal-open-mcp/instances/<sha256(projectPath)>.json` — its `port` is
   trusted only when its `pid` is still alive.
3. The deterministic hash of the project path (`20000 + sha256 % 10000`).

Readiness is "connected AND not compiling":

- HTTP 503, or a 2xx body with `compiling: true`, keeps the wait alive — the
  bridge is up but the editor is mid-compile.
- A 2xx body with `connected: false` is treated as not-yet-wired.
- A network error (e.g. `ECONNREFUSED` while the editor is still booting) is
  transient and keeps the wait alive.

The poller **fails fast** on a dead-bridge signature: if the editor process is
alive but the instance lock's heartbeat is stale (older than ~10s), the bridge
module failed to recompile and `/ping` will never recover. The command exits
`3` with a message telling you to fix the C++ errors and re-run.

Exit codes: `0` ready, `3` timeout / dead-bridge. With `--json`, the ready
envelope goes to stdout; the non-ready envelope goes to stderr (exit `3`).

## status

Reports project, plugin presence, resolved bridge port, instance lock summary,
and bridge readiness in one shot. This is the diagnostic surface for setup
troubleshooting — it never fails solely because the editor is down
(`stopped`/`unreachable` are successful reports, exit `0`), mirroring the MCP
`unreal_open_mcp_bridge_status` tool so the CLI and MCP agree on status tokens.

```sh
unreal-open-mcp-cli status [projectDir]
  [--port <n>]      # bridge port override (default: discovered, see below)
  [--no-probe]      # skip the live /ping probe (derive status from the lock)
```

`projectDir` resolves in this order: the positional arg, `--project <path>`,
`$UNREAL_PROJECT_PATH`, then the current working directory. A relative path is
absolutized against the current working directory before hashing.

The bridge port is resolved with the **same precedence the MCP server uses**
(see `wait-for-ready`): `--port`/`$UNREAL_OPEN_MCP_BRIDGE_PORT` > live instance
lock > deterministic hash. The instance lock + its classification are read from
`~/.unreal-open-mcp/instances/<sha256(projectPath)>.json`. Plugin presence is
probed under `<project>/Plugins/` (bridge + verify descriptors).

Unless `--no-probe` is passed, a single `GET /ping` probe is fired at
`http://127.0.0.1:<port>/ping`. The probe + the lock classification compose the
coarse bridge status:

- `running` — bridge connected, idle.
- `compiling` — bridge connected, editor compiling.
- `stopped` — editor not running OR bridge toggle off (no live listener).
- `unreachable` — editor process alive but the listener did not respond (a
  transient Live Coding / domain-reload window; retry shortly).
- `dead_bridge` — editor process alive but the bridge module failed to
  recompile, so `/ping` will never recover (fix the C++ errors and reload).

With `--no-probe`, the status is derived from the lock alone (`dead_bridge` if
the heartbeat is stale, `stopped` otherwise). Exit `0` on any report; exit `2`
only on a project-dir resolution error. With `--json`, the report envelope goes
to stdout.

## configure

Reads/writes the project-local Open MCP settings file
(`<project>/.unreal-open-mcp/settings.json` — the same file the bridge reads
`authMode` from, so there is one settings source). Local-only: no cloud host,
cloud URL, connection-mode, or token fields. The MVP key is the bridge port
override.

```sh
unreal-open-mcp-cli configure [projectDir]
  [--bridge-port <n>]     # set the bridge port override
  [--clear-bridge-port]   # clear (delete) the bridge port override
  [--dry-run]             # resolve + report, write nothing
```

`projectDir` resolves in this order: the positional arg, `--project <path>`,
`$UNREAL_PROJECT_PATH`, then the current working directory.

A re-run **deep-merges** the patch into the existing settings — unrelated keys
the bridge writes (`authMode`, `defaultGateMode`, ...) are preserved in place. A
missing or malformed file is treated as empty (a fresh object), so configure
never clobbers a hand-edited file with garbage. `--bridge-port` and
`--clear-bridge-port` are mutually exclusive; running configure with neither
prints the current settings (a read).

With `--json`, the result envelope goes to stdout; failure emits it on stderr
(exit `2`).

## The one-command loop

`open && wait-for-ready` is the default developer loop — launch the editor, then
block until the bridge is usable:

```sh
unreal-open-mcp-cli open --project /path/to/MyProject \
  && unreal-open-mcp-cli wait-for-ready --project /path/to/MyProject
```

After `wait-for-ready` exits `0`, the bridge is answering `/ping` and the MCP
server (spawned by your AI client via the `setup-mcp` snippet) can route tool
calls to it.

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
| `UE_ROOT` | Engine install root the `open` command launches the editor from (when `--engine-root` is not passed). |

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
