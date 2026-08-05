/**
 * Bundled-scenario integrity guard.
 *
 * Loads every shipped scenario JSON under `scenarios/unreal/` from disk and
 * runs it through the loader, asserting the invariants the suite relies on:
 * zero load errors, the core five are present, every scenario targets the
 * `unreal` profile, and every tool name referenced (agent_prompt + mcp_tool)
 * carries the `unreal_open_mcp_` prefix. This is the config-action drift
 * guard at the scenario level — a tool-name typo or an unknown step/action
 * verb fails the build here before an operator ever loads the suite.
 *
 * Run with: `npm run test:core`
 */

import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import { loadScenarios } from "./loader.ts";
import type { Scenario, ScenarioStep, SetupAction } from "./types.ts";

const TOOL_PREFIX = "unreal_open_mcp_";

// The test file lives at packages/core/src/; the scenarios ship at the
// validation-suite root under scenarios/unreal/.
const SCENARIOS_ROOT = path.resolve(
  import.meta.dirname,
  "..",
  "..",
  "..",
  "scenarios",
  "unreal",
);

/** Recursively collect every `.json` file under a directory. */
function collectJsonFiles(root: string): string[] {
  const out: string[] = [];
  if (!fs.existsSync(root)) return out;
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const full = path.join(root, entry.name);
    if (entry.isDirectory()) {
      out.push(...collectJsonFiles(full));
    } else if (entry.isFile() && entry.name.endsWith(".json")) {
      out.push(full);
    }
  }
  return out;
}

interface LoadedBundle {
  scenarios: Scenario[];
  errors: { source: string; message: string }[];
}

/**
 * Load + parse every bundled scenario file through the loader. Throws on a
 * raw JSON parse error (a malformed JSON file is a build break, not a soft
 * load error). Loader-level validation errors land in `errors`.
 */
function loadBundle(): LoadedBundle {
  const entries = collectJsonFiles(SCENARIOS_ROOT).sort().map((full) => {
    const content: unknown = JSON.parse(fs.readFileSync(full, "utf-8"));
    const rel = path.relative(SCENARIOS_ROOT, full).split(path.sep).join("/");
    return { source: rel, content };
  });
  return loadScenarios(entries);
}

/** Walk a step's actions and yield every `mcp_tool` tool name. */
function* mcpToolNames(step: ScenarioStep): Generator<string> {
  if (step.type !== "setup" || !step.actions) return;
  for (const a of step.actions as SetupAction[]) {
    if (a.action === "mcp_tool" && typeof a.tool === "string") yield a.tool;
  }
}

test("bundled scenarios: directory exists and ships the core five", () => {
  assert.ok(fs.existsSync(SCENARIOS_ROOT), `scenarios root missing: ${SCENARIOS_ROOT}`);
  const bundle = loadBundle();
  const ids = bundle.scenarios.map((s) => s.id);
  for (const id of [
    "core-ping",
    "core-actor-create",
    "core-gate-fail",
    "core-fix",
    "core-screenshot",
  ]) {
    assert.ok(ids.includes(id), `core scenario missing: ${id}`);
  }
});

test("bundled scenarios: every file loads with zero errors", () => {
  const res = loadBundle();
  assert.equal(res.errors.length, 0, res.errors.map((e) => `${e.source}: ${e.message}`).join("\n"));
});

test("bundled scenarios: every scenario targets the unreal profile", () => {
  for (const s of loadBundle().scenarios) {
    assert.equal(s.engineId, "unreal", `${s.id}: engineId must be "unreal"`);
  }
});

test("bundled scenarios: every referenced tool name carries the unreal_open_mcp_ prefix", () => {
  const bundle = loadBundle();
  const offenders: string[] = [];
  for (const s of bundle.scenarios) {
    for (const step of s.steps as ScenarioStep[]) {
      if (step.type === "agent_prompt" && typeof step.tool === "string") {
        if (!step.tool.startsWith(TOOL_PREFIX)) {
          offenders.push(`${s.id} › ${step.id}: agent_prompt tool "${step.tool}"`);
        }
      }
      for (const tool of mcpToolNames(step)) {
        if (!tool.startsWith(TOOL_PREFIX)) {
          offenders.push(`${s.id} › ${step.id}: mcp_tool tool "${tool}"`);
        }
      }
    }
  }
  assert.deepEqual(offenders, [], `tool names missing the ${TOOL_PREFIX} prefix:\n${offenders.join("\n")}`);
});

test("bundled scenarios: core-fail/fix staging uses a known safe-fix issue code", () => {
  // Guardrail: the gate-fail + fix scenarios' expected outcomes reference the
  // broken_soft_reference issue code + the clear_broken_soft_reference fix id,
  // which is the only Safe fix provider shipped. If either changes, this test
  // flags the drift so the scenario copy stays in sync with the fix catalog.
  const bundle = loadBundle();
  const prose = bundle.scenarios
    .filter((s) => s.id === "core-gate-fail" || s.id === "core-fix")
    .map((s) => JSON.stringify(s))
    .join("\n");
  assert.match(prose, /broken_soft_reference/, "gate-fail/fix must reference broken_soft_reference");
  assert.match(prose, /clear_broken_soft_reference/, "gate-fail/fix must reference clear_broken_soft_reference");
});
