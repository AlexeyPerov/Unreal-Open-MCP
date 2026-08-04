# Unreal Open MCP demo project

A minimal C++ Unreal Engine project that hosts the bridge plugin and serves as a
stable fixture root for end-to-end smokes and validation. It carries no game
logic — it exists so the MCP server, the bridge, and the gate/verify layer have
a reproducible project to target.

Requires **Unreal Engine 5.6 or newer** (developed against **5.8**).

## Layout

```
demo/
├─ UnrealOpenMcpDemo.uproject     # project descriptor (EngineAssociation 5.8)
├─ Source/
│  ├─ UnrealOpenMcpDemoEditor.Target.cs   # editor build target (smoke target)
│  ├─ UnrealOpenMcpDemo.Target.cs         # game (packaged) build target
│  └─ UnrealOpenMcpDemo/                  # primary runtime game module
│     ├─ UnrealOpenMcpDemo.Build.cs
│     └─ Private/UnrealOpenMcpDemoModule.cpp
├─ Config/DefaultEngine.ini       # default maps + project identity
├─ Content/                       # editor-generated assets (gitignored by default)
└─ .gitignore                     # Binaries / Intermediate / Saved / DerivedDataCache
```

The `Plugins/` folder is **not** checked in. Install the bridge into it (below)
— the demo never vendors a second copy of the plugins.

## 1) Install the bridge plugin

The `UnrealOpenMCP` and `UnrealOpenMCPVerify` plugins are **enabled** in the
`.uproject` already, but their source must be placed under `demo/Plugins/` so
UE can compile them. The supported path is the CLI installer, run from the
repository root:

```bash
unreal-open-mcp-cli install-plugin --project "$(pwd)/demo"
```

That copies `packages/bridge` → `demo/Plugins/UnrealOpenMCP` and
`packages/verify` → `demo/Plugins/UnrealOpenMCPVerify` (excluding `Binaries/`
and `Intermediate/`), and idempotently enables both in the `.uproject`. Re-run
after pulling plugin changes — the installer is a no-op when nothing changed.

For the dev loop (live edits to plugin source land without a re-copy), pass
`--symlink`:

```bash
unreal-open-mcp-cli install-plugin --project "$(pwd)/demo" --symlink
```

Manual install is documented in [Manual setup](../docs/manual-setup.md).

## 2) Open the project in the editor

Open `demo/UnrealOpenMcpDemo.uproject` in the Unreal Editor (double-click, or
`File ▸ Open` inside UE). On first open UE compiles the game module and both
plugins — wait for the status bar to settle.

CLI one-command loop:

```bash
unreal-open-mcp-cli open --project "$(pwd)/demo" && \
  unreal-open-mcp-cli wait-for-ready --project "$(pwd)/demo"
```

`open` launches the editor for the project; `wait-for-ready` blocks until the
bridge answers `/ping`.

## 3) Point the MCP server at the demo

Set `UNREAL_PROJECT_PATH` to the **absolute** path to this `demo/` directory
when configuring your AI client. The bridge binds a deterministic per-project
port derived from that path, so the server discovers it automatically — no
hardcoded port.

```bash
export UNREAL_PROJECT_PATH="/absolute/path/to/Unreal-Open-MCP/demo"
```

See [Manual setup](../docs/manual-setup.md) for the copy-paste MCP client config
snippets (Cursor, Claude Desktop, Claude Code, VS Code, Gemini CLI, Cline), and
[Architecture](../docs/architecture.md) for how the server routes calls to the
bridge.

## 4) Verify end-to-end

With the editor open and the plugin compiled, ask your AI client to run:

- `unreal_open_mcp_ping` — a ready response means the two halves are talking.
- `unreal_open_mcp_bridge_status` — a coarse health snapshot
  (`running` / `compiling` / `stopped` / `unreachable` / `dead_bridge`) with a
  recovery hint when the bridge is down.

For the scripted stdio smoke against the built server, see
[E2E smoke verification](../docs/architecture.md#e2e-smoke-verification). Pass
`--project /absolute/path/to/demo` (and `--port <n>` against a live editor) to
target this project.

## Smoke fixtures

Binary `.uasset` / `.umap` assets cannot be authored by hand, so `Content/`
ships empty and the level/map fixtures are **generated on first editor open**
(`Config/DefaultEngine.ini` points at the engine's default template map until a
project map is saved). Freeze the paths you depend on once they are stable:

- **Level:** save the default map as `/Game/Maps/SmokeLevel` and update
  `DefaultEngine.ini` to point `GameDefaultMap` / `EditorStartupMap` at it.
- **Actor stub:** optionally create a `BP_Smoke` Actor Blueprint under
  `/Game/Blueprints` for actor-create / spawn smokes.

Once stable, force-add the binary fixtures so they survive `Content/.gitignore`:

```bash
git add -f demo/Content/Maps/SmokeLevel.umap
git add -f demo/Content/Blueprints/BP_Smoke.uasset
```

## Engine association

The `.uproject` declares `"EngineAssociation": "5.8"` (the develop / CI engine).
To open it under another 5.6+ install, either let the launcher pick the right
engine, or change the association to `5.6` / `5.7` / `5.8` to match your local
install. The Open MCP plugins deliberately do **not** pin an `EngineVersion` in
their descriptors (per the UE-version decision), so they load across 5.6–5.8.
