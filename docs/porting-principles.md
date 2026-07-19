# Porting principles

Unreal Open MCP is a **port** of the [Unity Open MCP](https://github.com/AlexeyPerov/Unity-Open-MCP) architecture (stdio MCP + loopback HTTP bridge + gate/verify). The goal is to reuse proven patterns, not reinvent them.

Local planning crosswalk: `specs/porting-map.md` (gitignored).

---

## Anti-reinvention policy

Before designing or coding any non-trivial feature:

1. Check whether Unity Open MCP already solves it.
2. Read the relevant Unity source files (see porting map for the current phase).
3. Port with an explicit fidelity decision — do not silently diverge.

If you cannot find a Unity equivalent, mark the work **greenfield** and document why.

---

## Reference sources

| Source | Role | Where to read |
|---|---|---|
| **Unity Open MCP** | Canonical — *how* to build | Local: `/Users/alexeyperov/Projects/Unity-AI-Hub` · Public: [github.com/AlexeyPerov/Unity-Open-MCP](https://github.com/AlexeyPerov/Unity-Open-MCP) |
| **Unreal behavior reference** | Secondary — Unreal editor API and handler examples (read-only) | Listed per task in `specs/porting-map.md` (gitignored) |
| **specs/porting-map.md** | Per-phase file crosswalk | Gitignored local planning |

Use Unity Open MCP for transport, envelopes, gate workflow, tool routing, instance discovery, and CLI patterns. Use Unreal behavior reference files listed in the porting map per task for Unreal editor API usage, handler logic, and Blueprint/asset/level edge cases — never copy third-party SignalR/cloud transport stacks or .NET sidecar MCP hosts from those references. MCP tool names default to `unreal_open_mcp_*` (ADR-003); matching external tool names is optional.

---

## Fidelity rubric

| Tag | Meaning | When to use | Examples |
|---|---|---|---|
| **copy** | Port structure with minimal renames | Logic is engine-agnostic | `tool-router.ts`, `live-client.ts`, gate envelopes, instance discovery |
| **adapt** | Same pattern, different engine API | Core idea transfers; APIs differ | `BridgeHttpServer.cs` → C++ `GameThreadDispatcher` + UObject tools |
| **greenfield** | No Unity equivalent | Unreal-only concern | Blueprint compile loop, `.uasset` offline scope, C++ Live Coding gate |
| **skip** | Unity-only — do not port | Feature has no Unreal analogue | asmdef verify rules, Unity Hub deep links |

Every change area in a PR must declare its fidelity tag.

---

## Definition of Ready (phase / task)

A task is ready to implement when:

- [ ] Current phase is identified in the roadmap.
- [ ] `specs/porting-map.md` rows for that phase are read.
- [ ] Unity Open MCP reference files for the task are listed.
- [ ] Fidelity tags are assigned before coding starts.
- [ ] Unreal behavior reference files are listed when the porting-map row has an Unreal reference column entry (or explicitly marked `—`).

---

## Definition of Done (phase / task)

A task is done when:

- [ ] Unreal implementation matches the chosen fidelity intent.
- [ ] Intentional deltas from Unity are documented.
- [ ] Test parity note is filled (ported test, new equivalent, or justified gap).
- [ ] Tracked docs updated if behavior or contracts changed.
- [ ] PR description includes the reuse evidence checklist (see `.github/pull_request_template.md`).
- [ ] Early smoke tests pass where applicable (ping, typed-tool, and asset-family E2E routes — see [E2E smoke verification](architecture.md#e2e-smoke-verification)).

---

## Required evidence (PR / handoff)

Copy this block into PR descriptions or agent handoffs:

```markdown
### Unity-first evidence

- **Unity files consulted:** (paths)
- **Unreal behavior reference files consulted:** (paths, or — if none for this task)
- **Fidelity tags:** (area → copy|adapt|greenfield|skip)
- **Intentional deltas:** (bullets)
- **Test parity:** (ported from / equivalent added / gap + reason)
- **Tool naming:** unreal_open_mcp_* (default) | aligned with external tool names (optional — note which)
```

Full detail may also live in `specs/execution/P{n}/execution-plan.md` (gitignored).

---

## Do-not-port list (Unity-only)

| Unity feature | Reason |
|---|---|
| asmdef verify rules | Unity `.asmdef` has no Unreal counterpart |
| `unity_open_mcp_hub_*` tools | Unity Hub deep links; Hub deferred |
| IvanMurzak NuGet stack | Own bridge protocol (ADR-004) |
| In-process C# bridge | Unreal editor is C++; use C++ HTTP bridge |

See `specs/porting-map.md` for the full list.
