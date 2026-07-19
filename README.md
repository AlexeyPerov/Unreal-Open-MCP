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

> **Status:** Under active development. Core bridge and tool families are landing
> incrementally; setup docs and the full catalog are not yet complete.

## Key features

### Live bridge + offline reads (planned)

Prefer the live Editor via a loopback HTTP bridge; read project data from disk
when the editor is unavailable (narrow scope for binary `.uasset` assets).

> **Example:** "Bridge is offline — list Content Browser folders I can still
> inspect without opening the editor."

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

### Tool groups and skills (planned)

Keep the default prompt surface small; activate domains on demand. Project
skills teach agents the mutate → gate → fix loop.

> **Example:** "Reset tool groups, then activate only core editor and
> gate-and-verify."

### Native stdio MCP

No HTTP proxy or cloud dependency for Cursor and other native MCP clients —
fully self-hostable MIT stack.

> **Example:** point your MCP client at the local stdio server and call
> `unreal_open_mcp_ping`.

Requires **Unreal Engine 5.6+** (developed against **5.8**).

## Quick setup

Setup guides will land as the install path stabilizes.

1. **Manual / local checkout:** build and run from this repository (CLI +
   plugin + MCP server). Contributor-oriented notes live in
   [Architecture](docs/architecture.md).
2. **Agent / wizard setup:** planned — same shape as Unity Open MCP’s setup
   docs once the Unreal install path is ready.

## Documentation

For users:

- [API index](docs/api.md) — MCP, bridge, and contract documentation map.
- [Bridge HTTP](docs/api/bridge-http.md) — loopback bridge endpoints and envelopes.

For contributors:

- [Architecture](docs/architecture.md) — repository boundaries and runtime flow.
- [Porting principles](docs/porting-principles.md) — Unity-first porting protocol.

Detailed MCP tool catalog and setup guides will be added as features ship.

## Contributing

PRs welcome. See the docs above and package-level `AGENTS.md` files for local
development rules.

**License:** MIT — see [LICENSE](LICENSE).
