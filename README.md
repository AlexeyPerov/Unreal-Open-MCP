# Unreal Open MCP

[![MCP](https://badge.mcpx.dev 'MCP Server')](https://modelcontextprotocol.io/introduction)
[![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.6%2B-0E1128?style=flat&logo=unrealengine&logoColor=white&labelColor=333A41 'Unreal Engine 5.6+')](https://www.unrealengine.com/)
[![License](https://img.shields.io/badge/License-MIT-red.svg 'MIT License')](https://opensource.org/licenses/MIT)

**Unreal Open MCP** is local-first AI tooling for Unreal Engine game projects. Agents connect via stdio MCP (Cursor, Claude, Copilot, and others) and drive the Unreal Editor through a loopback HTTP bridge with a safe mutation workflow.

> **Status:** Pre-alpha — bootstrap in progress. Core bridge, tools, and CLI are not yet shipped.

## Planned features

- **Gate + verify workflow** — automatic validation, checkpoints, deltas, and targeted fixes before and after mutations.
- **Native stdio MCP** — no HTTP proxy or cloud dependency for Cursor and other native MCP clients.
- **Core editor tool families** — actor, level, asset, Blueprint, source, editor/reflection, screenshot, and ping tools (capability parity target).
- **Offline reads** — partial project introspection without a running editor (narrow scope for binary `.uasset` assets).
- **Tool groups + `manage_tools`** — keep the prompt surface small as tool count grows.
- **Open MIT stack** — fully self-hostable, no vendor lock-in.

Requires **Unreal Engine 5.6+** (developed against **5.8**).

## Architecture reference

This project ports the architecture of [Unity Open MCP](https://github.com/AlexeyPerov/Unity-Open-MCP) — stdio MCP server, loopback HTTP bridge, and gate/verify safety layer — adapted for Unreal Engine.

## Documentation

- [Architecture](docs/architecture.md) — repository boundaries and runtime flow.
- [API index](docs/api.md) — contract documentation map.
- [Porting principles](docs/porting-principles.md) — Unity-first porting protocol for contributors.

Detailed API docs (`docs/api/mcp-tools.md`, `docs/api/bridge-http.md`, setup guides) will be added as features ship.

## Contributing

PRs welcome. See the docs above and package-level `AGENTS.md` files for local development rules.

**License:** MIT — see [LICENSE](LICENSE).
