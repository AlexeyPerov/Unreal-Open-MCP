# Unreal Open MCP

[![Status](https://img.shields.io/badge/Status-Under%20Development-orange?style=flat-square 'Under Development')](https://github.com/AlexeyPerov/Unreal-Open-MCP)
[![](https://badge.mcpx.dev?status=on 'MCP Enabled')](https://modelcontextprotocol.io/introduction)
[![](https://img.shields.io/badge/Unreal%20Engine-5.6%2B-0E1128?style=flat&logo=unrealengine&logoColor=white&labelColor=333A41 'Unreal Engine')](https://www.unrealengine.com/)
[![](https://img.shields.io/badge/Node.js-339933?style=flat&logo=nodedotjs&logoColor=white 'Node.js')](https://nodejs.org/en/download/)
[![](https://img.shields.io/github/stars/AlexeyPerov/Unreal-Open-MCP 'Stars')](https://github.com/AlexeyPerov/Unreal-Open-MCP/stargazers)
[![](https://img.shields.io/github/last-commit/AlexeyPerov/Unreal-Open-MCP 'Last Commit')](https://github.com/AlexeyPerov/Unreal-Open-MCP/commits/master)
[![](https://img.shields.io/badge/License-MIT-red.svg 'MIT License')](https://opensource.org/licenses/MIT)

<p align="center">
  <img src="https://img.shields.io/badge/🚧_Under_Development-orange?style=for-the-badge" alt="Under Development">
</p>

Unreal Open MCP gives AI agents a typed, safety-gated tool surface for Unreal
Engine projects.

Based on the architecture and workflows of
[Unity Open MCP](https://github.com/AlexeyPerov/Unity-Open-MCP) — stdio MCP
server, loopback HTTP bridge, and gate/verify safety layer — adapted for
Unreal Engine.

---
Part of Open MCP toolset
---
[![Unity Open MCP](https://img.shields.io/badge/Unity-Open%20MCP-000000?style=flat&logo=unity&logoColor=white)](https://github.com/AlexeyPerov/Unity-Open-MCP) [![Unreal Open MCP](https://img.shields.io/badge/Unreal-Open%20MCP-0E1128?style=flat&logo=unrealengine&logoColor=white)](https://github.com/AlexeyPerov/Unreal-Open-MCP) [![Godot Open MCP](https://img.shields.io/badge/Godot-Open%20MCP-478CBF?style=flat&logo=godotengine&logoColor=white)](https://github.com/AlexeyPerov/Godot-Open-MCP)
---

> **Status:** Under active development. Core bridge and tool families are landing
> incrementally; setup docs and the full catalog are not yet complete.

## Key features

### Live bridge + offline reads

Prefer the live Editor via a loopback HTTP bridge; read project data from disk
when the editor is unavailable. Offline reads cover project files, source text,
and the editor log (compile errors when the bridge is dead) — binary `.uasset`
assets are not parsed offline.

> **Example:** "The bridge is dead — read the compile errors from the editor
> log and inspect the offending source file without opening the editor."

### Typed editor tool families

Actors, levels, assets / Content Browser, Blueprint, source, editor reflection,
screenshots, and ping — growing toward full editor capability coverage.

> **Example:** "Find the PlayerStart actor, duplicate it, and parent the copy
> under the Spawns folder."

### Safety-gated mutations

Mutations run `checkpoint → mutate → validate → delta`, with targeted fixes —
so agents can stop before a “successful” edit leaves the project broken.

> **User:** Delete that Blueprint.  
> **Agent:** Checking impact…  
> **Gate:** Removing it would break soft references on `Level1`.  
> **Agent:** Unreal Open MCP flagged that in the gate preview. I am **not**
> deleting it without your confirmation.

### Gate, verify, and apply_fix

Validate edits, create checkpoints, compute deltas, and preview/apply safe fixes
through dedicated meta-tools.

> **Example:** "Validate the last edit, show the delta, and dry-run any safe
> fixes."

### Tool groups and session visibility

The default `tools/list` surface is intentionally small — `core` connectivity
plus the always-visible discovery / recovery tools — so an agent's prompt is
not bloated. Activate a family on demand via `manage_tools`.

> **Example:** "Reset tool groups, then activate `typed-editor` and
> `gate-and-verify`." See [Tool groups and session visibility](docs/api/tool-groups.md).

An agent skill at [`skills/unreal-open-mcp/SKILL.md`](skills/unreal-open-mcp/SKILL.md)
teaches the discover → activate → mutate → gate → fix loop: capabilities first,
tool-group activation, scoped `paths_hint`, and offline triage when the bridge is
dead.

### Native stdio MCP

No HTTP proxy or cloud dependency for Cursor and other native MCP clients —
fully self-hostable MIT stack.

> **Example:** point your MCP client at the local stdio server and call
> `unreal_open_mcp_ping`.

Requires **Unreal Engine 5.6+** (developed against **5.8**).

## Quick setup

1. **Manual setup:** install the bridge plugin and point your AI client at the
   stdio MCP server by hand — copy-paste JSON snippets for Cursor, Claude
   Desktop, Claude Code, VS Code, Gemini CLI, and Cline in
   [Manual setup](docs/manual-setup.md).
2. **CLI one-command path:** the `unreal-open-mcp-cli` tool can install the
   plugin (`install-plugin`), write the MCP client config (`setup-mcp`), launch
   the editor (`open`), and wait for the bridge (`wait-for-ready`). See
   [Architecture](docs/architecture.md) for contributor-oriented notes and the
   [`cli/` README](cli/README.md) for the command reference.
3. **Agent / wizard setup:** a guided desktop wizard is planned.

## Documentation

For users:

- [Manual setup](docs/manual-setup.md) — install the plugin and wire an AI client by hand (Cursor, Claude Desktop, Claude Code, VS Code, Gemini CLI, Cline).
- [API index](docs/api.md) — MCP, bridge, and contract documentation map.
- [MCP tools catalog](docs/api/mcp-tools.md) — every tool's route (`live`/`offline`/`local`/`batch`), mutation + gate contract, and offline coverage.
- [Tool groups and session visibility](docs/api/tool-groups.md) — group catalog, default surface, and per-session activation.
- [Bridge HTTP](docs/api/bridge-http.md) — loopback bridge endpoints and envelopes.

For contributors:

- [Architecture](docs/architecture.md) — repository boundaries and runtime flow.
- [Porting principles](docs/porting-principles.md) — Unity-first porting protocol.
- [Demo project](demo/README.md) — minimal C++ project that hosts the bridge and serves as a smoke / validation fixture root.

The [MCP tools catalog](docs/api/mcp-tools.md) is the per-tool reference;
[Manual setup](docs/manual-setup.md) covers the install path.

## Contributing

PRs welcome. See the docs above and package-level `AGENTS.md` files for local
development rules.

**License:** MIT — see [LICENSE](LICENSE).
