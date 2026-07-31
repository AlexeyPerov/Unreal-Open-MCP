# Tool groups and session visibility

The MCP tool surface spans many families (actor, level, assets, gate, editor,
Blueprint, source, …). Without grouping, every `tools/list` dumps the full
catalog into the agent's prompt. Tool groups keep the default surface small and
let an agent activate a family on demand.

## How it works

Every registered tool maps to a **group id** via the canonical catalog in
`mcp-server/src/capabilities/tool-groups.ts`. `tools/list` filters the
registered set through a per-session visibility store
(`mcp-server/src/tool-session-state.ts`) so only the active groups' tools are
advertised.

A fresh session activates only the **default-on** groups. For Unreal Open MCP
the lean baseline is `core` alone (the connectivity probe) plus a small set of
**always-visible** meta / recovery tools that bypass the filter entirely.
Everything else activates on demand.

### Visibility rules (precedence high → low)

1. The tool is in the **always-visible** allow-list → always shown (an agent
   can reach it even with every group torn down).
2. The tool has **no group assignment** (`groupFor` returns `null`) → always
   shown (defensive — matches the catalog intent for meta / recovery tools).
3. The tool's group is in the session's **active set** → shown.
4. Otherwise → hidden.

### Always-visible tools

| Tool | Why it is always visible |
|---|---|
| `unreal_open_mcp_ping` | The connectivity probe an agent needs to re-check the bridge after tearing down `core`. |
| `unreal_open_mcp_capabilities` | Discovery — an agent must be able to ask what exists before activating a group. |
| `unreal_open_mcp_bridge_status` | Operator / recovery health snapshot; must survive a `core` teardown. |
| `unreal_open_mcp_read_compile_errors` | The one offline channel that works when the bridge assembly is dead. |
| `unreal_open_mcp_manage_tools` | The group activation mutator itself (so an agent can always re-activate a group). |

## Group catalog

| Group | Default | Contents |
|---|---|---|
| `core` | **on** | `ping` — the essential connectivity probe. |
| `gate-and-verify` | off | `validate_edit`, `checkpoint_create`, `delta`, `apply_fix` — the verify surface. |
| `typed-editor` | off | The full authoring surface: actor, level, asset, material, editor, selection, console, reflection, screenshot, Blueprint, and source families. |
| `diagnostics` | off | Reserved for profiler / per-frame diagnostic reads (empty today; reserved so future profiler tools land in a stable group). |

## Activating a group

Group activation is driven by the `unreal_open_mcp_manage_tools` meta-tool
(`activate` / `deactivate` / `reset` / `list_groups`). Activation is
**ephemeral and per-session** — it lives in the stdio MCP server's memory, not
on disk, and resets to the default-on set on every server restart. One stdio
server process has one connected client and one session store.

> **Example workflow:** "Reset tool groups, then activate `typed-editor` and
> `gate-and-verify`." The next `tools/list` advertises the `core` tools, the
> full typed-editor surface, and the verify surface — nothing else.

## Capabilities surface

`unreal_open_mcp_capabilities` reports a session-agnostic `toolGroups` block:
the catalog (which groups exist, their default-on flag) plus the per-group
roster derived from the registered tool set. Per-session activation state is
**not** reported here — that lives in `manage_tools` `list_groups`.

## Intentional deltas from Unity Open MCP

Unreal Open MCP ports the group-system skeleton from
[Unity Open MCP](https://github.com/AlexeyPerov/Unity-Open-MCP) with these
deltas:

- **Default-on set is `core` only.** Unity also defaults `gate-and-verify`;
  the Unreal roadmap pins the leaner `core`-only baseline.
- **No domain package auto-activation.** Unity auto-activates a group when its
  Unity package is detected; Unreal domain packs are a later concern and there
  is no package-detection auto-activation yet.
- **Smaller catalog.** Only the groups the Unreal surface needs today
  (`core` / `gate-and-verify` / `typed-editor` / `diagnostics`). No profiler /
  hub / senses / nav / input / … groups until those tools exist.
