# Manual setup

Wire an AI client to an Unreal Engine project by hand — no CLI required. If you
already use the CLI, `unreal-open-mcp-cli setup-mcp` writes these same JSON
snippets for you; this page is the copy-paste path for anyone who prefers to
edit the config file directly.

Unreal Open MCP has two halves you must connect:

- **The editor side** — the `UnrealOpenMCP` bridge plugin (plus its `UnrealOpenMCPVerify`
  dependency) installed into your project, exposing a loopback HTTP bridge.
- **The AI side** — a small Node stdio MCP server your AI client launches and
  talks to. The server forwards tool calls to the bridge.

The steps below cover each half in turn, then verify the two are talking.

## Prerequisites

- **Unreal Engine 5.6 or newer** (developed against 5.8).
- **Node.js 18 or newer** — required only so your AI client can launch the
  server via `npx`. Install from <https://nodejs.org/> (LTS), restart your
  terminal, and verify with `node --version`.
- **An AI client that supports stdio MCP servers** — Cursor, Claude Desktop,
  Claude Code, VS Code (Copilot), Gemini CLI, or Cline. Copy-paste snippets for
  each live further down.
- **Your project's absolute path** — the folder that contains the `.uproject`
  file (for example `/Users/me/Projects/MyGame` or
  `C:\Users\me\Projects\MyGame`). You will paste it into the config as
  `UNREAL_PROJECT_PATH`.

## 1) Install the bridge plugin into the project

The bridge ships as an Unreal plugin under
`packages/bridge/` in this repository, with its companion
verify plugin under `packages/verify/`. Copy both into your project's
`Plugins/` folder so the folder layout is:

```
<project>/
├─ MyGame.uproject
└─ Plugins/
   ├─ UnrealOpenMCP/        ← bridge (from packages/bridge/)
   │  └─ UnrealOpenMCP.uplugin
   └─ UnrealOpenMCPVerify/  ← verify  (from packages/verify/)
      └─ UnrealOpenMCPVerify.uplugin
```

If your project has no `Plugins/` folder yet, create it. Copy mode is
recommended over symlinks for a clean install (symlinks are a dev-loop
convenience and need extra permissions on Windows).

> Prefer the CLI for this step? `unreal-open-mcp-cli install-plugin --project
> <project>` copies both plugins and enables them in the `.uproject`
> idempotently — the same outcome as the manual steps below.

### Enable the plugins in the `.uproject`

Open `<project>/MyGame.uproject` in a text editor and make sure the `Plugins`
array enables both. A minimal descriptor looks like:

```json
{
  "FileVersion": 3,
  "EngineAssociation": "5.8",
  "Plugins": [
    { "Name": "UnrealOpenMCP", "Enabled": true },
    { "Name": "UnrealOpenMCPVerify", "Enabled": true }
  ]
}
```

If a `Plugins` array already lists other plugins, just add the two entries above
to it. If you launch the editor from this descriptor, the plugins are enabled
automatically on next open.

## 2) Configure your AI client

Each client reads an MCP server config from a specific file. Find yours in the
table, copy the matching snippet, replace `/absolute/path/to/project` with your
project's absolute path, and save the file.

| Client | Config file | Body key |
|---|---|---|
| Cursor | `<project>/.cursor/mcp.json` | `mcpServers` |
| Claude Desktop | OS-global — see below | `mcpServers` |
| Claude Code | `<project>/.mcp.json` | `mcpServers` |
| VS Code (Copilot) | `<project>/.vscode/mcp.json` | `servers` |
| Gemini CLI | `<project>/.gemini/settings.json` | `mcpServers` |
| Cline | OS-global — see below | `mcpServers` |

**Claude Desktop** config path by OS:

- **Windows:** `%APPDATA%\Claude\claude_desktop_config.json`
- **macOS:** `~/Library/Application Support/Claude/claude_desktop_config.json`
- **Linux:** `~/.config/Claude/claude_desktop_config.json`

**Cline** config path by OS:

- **Windows:** `%APPDATA%\Code\User\globalStorage\saoudrizwan.claude-dev\settings\cline_mcp_settings.json`
- **macOS:** `~/Library/Application Support/Code/User/globalStorage/saoudrizwan.claude-dev/settings/cline_mcp_settings.json`
- **Linux:** `~/.config/Code/User/globalStorage/saoudrizwan.claude-dev/settings/cline_mcp_settings.json`

### Snippets

Every snippet below is **identical in shape** — only the wrapping body key
differs. The server entry key is always `unreal-open-mcp`. Replace the two
placeholders:

- `unreal-open-mcp@0.0.1` — pin the version you are running (see the
  `version.json` at the repo root, or whatever you publish/install). The
  important part is `unreal-open-mcp` (the stdio server package), **not**
  `unreal-open-mcp-cli` (the setup/ops tool).
- `/absolute/path/to/project` — your project root (the `.uproject` parent).

#### Cursor, Claude Desktop, Claude Code, Gemini CLI, Cline (`mcpServers`)

```json
{
  "mcpServers": {
    "unreal-open-mcp": {
      "command": "npx",
      "args": ["-y", "unreal-open-mcp@0.0.1"],
      "env": {
        "UNREAL_PROJECT_PATH": "/absolute/path/to/project"
      }
    }
  }
}
```

#### VS Code / Copilot (`servers`)

VS Code uses `servers` instead of `mcpServers`:

```json
{
  "servers": {
    "unreal-open-mcp": {
      "command": "npx",
      "args": ["-y", "unreal-open-mcp@0.0.1"],
      "env": {
        "UNREAL_PROJECT_PATH": "/absolute/path/to/project"
      }
    }
  }
}
```

> **Keep siblings.** If the config file already lists other MCP servers, add
> only the `unreal-open-mcp` entry — do not wipe the rest of the file. The CLI's
> `setup-mcp` deep-merges for you; by hand, just preserve what is there.

### Optional: pin the bridge port

By default the bridge binds a **deterministic per-project port** derived from the
project path, and the MCP server discovers it the same way — no hardcoded port.
You usually do not need to set one. If you want to force a specific port (for
example to match a firewall rule), add it to the snippet's `env`:

```json
      "env": {
        "UNREAL_PROJECT_PATH": "/absolute/path/to/project",
        "UNREAL_OPEN_MCP_BRIDGE_PORT": "23456"
      }
```

Port discovery order (the MCP server and the CLI use the same precedence):

1. `UNREAL_OPEN_MCP_BRIDGE_PORT` env var (override wins).
2. The live instance lock at
   `~/.unreal-open-mcp/instances/<sha256(projectPath)>.json` — its `port` is
   trusted only while its `pid` is still alive.
3. The deterministic hash of the project path (`20000 + sha256 % 10000`).

There is no hardcoded port anywhere in the stack.

### Alternative server commands

The snippets above use `npx -y unreal-open-mcp@<version>`, which downloads the
server on first launch (the first run can take 10–60 seconds; later runs are
fast). Two alternatives:

- **Global install:** `npm install -g unreal-open-mcp`, then set
  `"command": "unreal-open-mcp"` with an empty `args` array.
- **Local checkout:** build the `mcp-server/` package (`npm install && npm run
  build`) and point at the built entry directly:

  ```json
  {
    "mcpServers": {
      "unreal-open-mcp": {
        "command": "node",
        "args": ["/absolute/path/to/Unreal-Open-MCP/mcp-server/dist/index.js"],
        "env": {
          "UNREAL_PROJECT_PATH": "/absolute/path/to/project"
        }
      }
    }
  }
  ```

  These are exactly the shapes `setup-mcp --server-command` writes
  (`unreal-open-mcp` for a global install, `node` for the monorepo-dev case).

## 3) Open the editor and verify

1. Open the **same** project (`UNREAL_PROJECT_PATH`) in the Unreal Editor.
2. Let it fully load and compile the plugin modules — wait for the status bar to
   settle.
3. Restart your AI client so it re-reads the MCP config from step 2.
4. Ask the client to list its MCP tools — `unreal_open_mcp_ping` should appear.
5. Run `unreal_open_mcp_ping`. A ready response means the two halves are
   talking. For a fuller health check, run `unreal_open_mcp_bridge_status` — it
   reports a coarse status token (`running` / `compiling` / `stopped` /
   `unreachable` / `dead_bridge`) and a recovery hint when the bridge is down.

> **CLI one-command loop:** `unreal-open-mcp-cli open --project <project> && \
> unreal-open-mcp-cli wait-for-ready --project <project>` launches the editor
> then blocks until the bridge answers `/ping`. Optional — you can open the
> editor yourself.

## What to try next

- Read the [MCP tools catalog](api/mcp-tools.md) for every tool's route
  (`live` / `offline` / `local` / `batch`) and its mutation + gate contract.
- Read [Tool groups and session visibility](api/tool-groups.md) for the lean
  default `tools/list` surface and how to activate a tool family on demand.
- Load the agent skill at [`skills/unreal-open-mcp/SKILL.md`](../skills/unreal-open-mcp/SKILL.md)
  — it teaches the discover → activate → mutate → gate → fix loop, including
  offline triage when the bridge is dead.

## Troubleshooting

- **Tools do not appear in the client** — restart the client so it reloads the
  config, and confirm `UNREAL_PROJECT_PATH` is the absolute project root (a
  relative path is rejected).
- **`unreal_open_mcp_ping` errors with `bridge_offline`** — the MCP server is
  up but cannot reach the editor. Open the project in the editor and let the
  plugin compile. Run `unreal_open_mcp_bridge_status` for a coarse diagnosis.
- **`bridge_status` reports `dead_bridge`** — the editor is running but the
  bridge module failed to recompile (a bad C++ edit or a Live Coding failure).
  Its `recoveryHint` points at `unreal_open_mcp_read_compile_errors`, the
  offline editor-log reader that works with the bridge assembly dead. Fix the
  C++ errors and reload.
- **Wrong server package** — the snippet must launch `unreal-open-mcp` (the
  stdio server), not `unreal-open-mcp-cli` (the setup/ops tool). The CLI wraps
  the server; it is not the server itself.
