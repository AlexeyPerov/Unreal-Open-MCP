import test from "node:test";
import assert from "node:assert/strict";

import { buildCapabilities, type BuildCapabilitiesDeps } from "./build-capabilities.js";
import {
  RULE_CATALOG,
  FIX_CATALOG,
  implementedRules,
  plannedRules,
  implementedFixes,
} from "./rule-catalog.js";
import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// ---------------------------------------------------------------------------
// Test fixtures — stand in for ALL_TOOLS without importing production modules
// that have cross-file runtime imports (strip-types safe).
// ---------------------------------------------------------------------------

const FIXTURE_TOOLS: Tool[] = [
  {
    name: "unreal_open_mcp_ping",
    description: "Bridge health check.",
    inputSchema: { type: "object", properties: {}, additionalProperties: false },
  },
  {
    name: "unreal_open_mcp_actor_find",
    description: "Find actors in the current level.",
    inputSchema: { type: "object", properties: {} },
  },
  {
    name: "unreal_open_mcp_validate_edit",
    description: "Scoped health check without a preceding mutation.",
    inputSchema: { type: "object", required: ["paths"], properties: {} },
  },
  {
    name: "unreal_open_mcp_apply_fix",
    description: "Apply a verify rule fix action.",
    inputSchema: { type: "object", required: ["issue_id"], properties: {} },
  },
  {
    name: "unreal_open_mcp_capabilities",
    description: "Discover the full capability surface.",
    inputSchema: { type: "object", properties: {} },
  },
];

const DEPS: BuildCapabilitiesDeps = {
  tools: FIXTURE_TOOLS,
  rules: RULE_CATALOG,
  fixes: FIX_CATALOG,
};

// ---------------------------------------------------------------------------
// Rule catalog — pins the v1 implemented surface (P3.2 / P3.3 / P3.4) and
// the planned roadmap. Fails on any rule drift between this catalog and the
// verify package's *IssueCodes.h headers.
// ---------------------------------------------------------------------------

test("rule catalog contains exactly the three implemented v1 rules", () => {
  const ids = implementedRules().map((r) => r.id);
  assert.deepEqual(ids.sort(), [
    "broken_soft_references",
    "compile_errors",
    "missing_blueprint_parents",
  ]);
});

test("implemented rules declare issue codes with severities", () => {
  for (const rule of implementedRules()) {
    assert.ok(
      rule.issues.length > 0,
      `${rule.id} should declare at least one issue code`,
    );
    for (const issue of rule.issues) {
      assert.ok(issue.code, "issue must have a code");
      assert.ok(
        issue.severity === "Error" || issue.severity === "Warning",
        `${rule.id}/${issue.code} severity must be Error or Warning`,
      );
      assert.ok(Array.isArray(issue.fixIds), "fixIds must be an array");
    }
  }
});

test("broken_soft_references rule emits broken_soft_reference issue", () => {
  const rule = RULE_CATALOG.find((r) => r.id === "broken_soft_references");
  assert.ok(rule, "broken_soft_references rule must be in the catalog");
  assert.equal(rule!.implemented, true);
  assert.equal(rule!.status, "implemented");
  const codes = rule!.issues.map((i) => i.code);
  assert.ok(codes.includes("broken_soft_reference"));
  // Pin severity — an unresolved soft target is a real breakage (Error).
  const issue = rule!.issues.find((i) => i.code === "broken_soft_reference");
  assert.equal(issue!.severity, "Error");
});

test("missing_blueprint_parents rule emits missing_blueprint_parent issue", () => {
  const rule = RULE_CATALOG.find((r) => r.id === "missing_blueprint_parents");
  assert.ok(rule, "missing_blueprint_parents rule must be in the catalog");
  assert.equal(rule!.implemented, true);
  assert.equal(rule!.status, "implemented");
  const codes = rule!.issues.map((i) => i.code);
  assert.ok(codes.includes("missing_blueprint_parent"));
  const issue = rule!.issues.find((i) => i.code === "missing_blueprint_parent");
  assert.equal(issue!.severity, "Error");
  // No fix provider for this code in v1 — clearing a missing parent is rarely
  // Safe. The catalog must reflect that honestly.
  assert.deepEqual(issue!.fixIds, []);
});

test("compile_errors rule emits compile_error issue", () => {
  const rule = RULE_CATALOG.find((r) => r.id === "compile_errors");
  assert.ok(rule, "compile_errors rule must be in the catalog");
  assert.equal(rule!.implemented, true);
  assert.equal(rule!.status, "implemented");
  const codes = rule!.issues.map((i) => i.code);
  assert.ok(codes.includes("compile_error"));
  const issue = rule!.issues.find((i) => i.code === "compile_error");
  assert.equal(issue!.severity, "Error");
  // No Safe fix — a compile error is an author-side defect the agent must
  // edit source to clear, not a Safe automated rewrite.
  assert.deepEqual(issue!.fixIds, []);
});

test("planned rules carry status and guidance, no hard errors", () => {
  const planned = plannedRules();
  assert.ok(planned.length >= 1, "should have at least one planned rule");
  for (const rule of planned) {
    assert.equal(rule.implemented, false);
    assert.equal(rule.status, "planned");
    assert.ok(
      typeof rule.guidance === "string" && rule.guidance.length > 0,
      `${rule.id} planned rule must carry guidance`,
    );
    assert.deepEqual(rule.issues, []);
  }
});

test("content_path_hygiene is the planned v1 stub", () => {
  const plannedIds = plannedRules().map((r) => r.id);
  assert.ok(
    plannedIds.includes("content_path_hygiene"),
    "content_path_hygiene planned stub must be in the catalog",
  );
  // v1 implemented rules must NOT be in the planned list.
  const implementedIds = implementedRules().map((r) => r.id);
  for (const id of implementedIds) {
    assert.ok(!plannedIds.includes(id), `${id} must not be planned`);
  }
});

// ---------------------------------------------------------------------------
// Explainability — every implemented-rule issue descriptor must carry a stable
// machine-readable rootCause + a clean remediation playbook. These mirror the
// rootCause taxonomy the C++ rule issue mappers stamp into FVerifyIssue, so
// scan_paths / validate_edit responses and this catalog classify findings
// identically. An agent can branch recovery programmatically on rootCause.
// ---------------------------------------------------------------------------

const STABLE_ROOT_CAUSES = new Set([
  "missing_soft_reference",
  "missing_blueprint_class",
  "build_blocker",
]);

// Forbidden tokens in user-visible remediation copy (AGENTS.md
// §No internal references). None should ever leak into remediation text.
const FORBIDDEN_INTERNAL_TOKENS = [
  "P3", "M3", "execution-plan", "specs/", "backlog-",
];

test("every implemented-rule issue carries a stable rootCause", () => {
  for (const rule of implementedRules()) {
    for (const issue of rule.issues) {
      assert.ok(
        typeof issue.rootCause === "string" && issue.rootCause.length > 0,
        `${rule.id}/${issue.code} must declare a rootCause`,
      );
      assert.ok(
        STABLE_ROOT_CAUSES.has(issue.rootCause!),
        `${rule.id}/${issue.code} rootCause '${issue.rootCause}' is not in the stable taxonomy`,
      );
    }
  }
});

test("every implemented-rule issue carries remediation guidance", () => {
  for (const rule of implementedRules()) {
    for (const issue of rule.issues) {
      assert.ok(
        typeof issue.remediation === "string" && issue.remediation.length > 0,
        `${rule.id}/${issue.code} must declare remediation guidance`,
      );
    }
  }
});

test("remediation copy is clean of internal IDs", () => {
  for (const rule of implementedRules()) {
    for (const issue of rule.issues) {
      for (const token of FORBIDDEN_INTERNAL_TOKENS) {
        assert.ok(
          !(issue.remediation ?? "").includes(token),
          `${rule.id}/${issue.code} remediation leaks internal token '${token}'`,
        );
      }
    }
  }
});

test("capabilities surfaces rootCause and remediation on issue descriptors", () => {
  // buildCapabilities passes the catalog through, so the capabilities surface
  // carries the explainability fields verbatim.
  const caps = buildCapabilities(DEPS, { kind: "rules" });
  const rule = caps.rules.find((r) => r.id === "broken_soft_references");
  assert.ok(rule);
  const issue = rule!.issues.find((i) => i.code === "broken_soft_reference");
  assert.ok(issue);
  assert.equal(issue!.rootCause, "missing_soft_reference");
  assert.ok(
    typeof issue!.remediation === "string" && issue!.remediation!.length > 0,
  );
});

// ---------------------------------------------------------------------------
// Fix catalog — pins the v1 implemented fix (P3.7 clear_broken_soft_reference)
// and the issue → fix linkage the apply_fix flow keys on.
// ---------------------------------------------------------------------------

test("clear_broken_soft_reference fix is registered and Safe", () => {
  const fix = implementedFixes().find((f) => f.id === "clear_broken_soft_reference");
  assert.ok(fix, "clear_broken_soft_reference must be in the implemented fix surface");
  assert.equal(fix!.implemented, true);
  assert.equal(fix!.safe, true);
  assert.ok(fix!.rules.includes("broken_soft_references"));
  assert.ok(fix!.issueCodes.includes("broken_soft_reference"));
});

test("broken_soft_reference issue maps to clear_broken_soft_reference fix", () => {
  const rule = RULE_CATALOG.find((r) => r.id === "broken_soft_references");
  assert.ok(rule);
  const issue = rule!.issues.find((i) => i.code === "broken_soft_reference");
  assert.ok(issue);
  assert.deepEqual(issue!.fixIds, ["clear_broken_soft_reference"]);
});

test("every catalog fix declares a Safe flag honestly", () => {
  // The catalog value must match the provider's Describe().bSafe at
  // registration time (see packages/verify/AGENTS.md §Fixes). When a new
  // fix ships, the author MUST set Safe deliberately — the catalog drift
  // guard catches an undefined Safe flag.
  for (const fix of FIX_CATALOG) {
    assert.equal(
      typeof fix.safe,
      "boolean",
      `${fix.id} must declare safe: boolean`,
    );
  }
});

// ---------------------------------------------------------------------------
// buildCapabilities — full surface
// ---------------------------------------------------------------------------

test("buildCapabilities returns tools, rules, and fixes in one call", () => {
  const caps = buildCapabilities(DEPS);
  assert.ok(caps.tools.length > 0);
  assert.ok(caps.rules.length > 0);
  assert.ok(caps.fixes.length > 0);
});

test("every registered tool appears as implemented with its schema", () => {
  const caps = buildCapabilities(DEPS);
  assert.equal(caps.tools.length, FIXTURE_TOOLS.length);
  for (const tool of caps.tools) {
    assert.ok(tool.name, "tool must have a name");
    assert.ok(tool.inputSchema, `${tool.name} must carry inputSchema`);
    assert.equal(typeof tool.routePolicy, "string");
    assert.equal(typeof tool.category, "string");
    assert.equal(tool.implemented, true);
  }
});

test("capabilities tool itself is in the implemented surface under capability-discovery", () => {
  const caps = buildCapabilities(DEPS);
  const found = caps.tools.find((t) => t.name === "unreal_open_mcp_capabilities");
  assert.ok(found, "unreal_open_mcp_capabilities must be discoverable");
  assert.equal(found!.implemented, true);
  assert.equal(found!.category, "capability-discovery");
});

test("capabilities tool routes local (no bridge round-trip)", () => {
  const caps = buildCapabilities(DEPS);
  const found = caps.tools.find((t) => t.name === "unreal_open_mcp_capabilities");
  assert.ok(found);
  assert.equal(found!.routePolicy, "local");
});

test("non-capabilities tools route live by default", () => {
  const caps = buildCapabilities(DEPS);
  const ping = caps.tools.find((t) => t.name === "unreal_open_mcp_ping");
  assert.ok(ping);
  assert.equal(ping!.routePolicy, "live");

  const validate = caps.tools.find((t) => t.name === "unreal_open_mcp_validate_edit");
  assert.ok(validate);
  assert.equal(validate!.routePolicy, "live");
});

test("counts reflect implemented vs planned split", () => {
  const caps = buildCapabilities(DEPS);
  assert.equal(caps.counts.toolsImplemented, FIXTURE_TOOLS.length);
  assert.equal(caps.counts.rulesImplemented, implementedRules().length);
  assert.equal(caps.counts.rulesPlanned, plannedRules().length);
  assert.equal(caps.counts.fixesImplemented, implementedFixes().length);
});

// ---------------------------------------------------------------------------
// buildCapabilities — filters
// ---------------------------------------------------------------------------

test("kind=rules returns only rules", () => {
  const caps = buildCapabilities(DEPS, { kind: "rules" });
  assert.equal(caps.tools.length, 0);
  assert.ok(caps.rules.length > 0);
  assert.equal(caps.fixes.length, 0);
});

test("kind=tools returns only tools", () => {
  const caps = buildCapabilities(DEPS, { kind: "tools" });
  assert.ok(caps.tools.length > 0);
  assert.equal(caps.rules.length, 0);
  assert.equal(caps.fixes.length, 0);
});

test("kind=fixes returns only fixes", () => {
  const caps = buildCapabilities(DEPS, { kind: "fixes" });
  assert.equal(caps.tools.length, 0);
  assert.equal(caps.rules.length, 0);
  assert.ok(caps.fixes.length > 0);
});

test("includePlanned=false drops planned rules", () => {
  const caps = buildCapabilities(DEPS, { includePlanned: false });
  for (const r of caps.rules) assert.equal(r.implemented, true);
  for (const f of caps.fixes) assert.equal(f.implemented, true);
  assert.equal(caps.counts.rulesPlanned, 0);
});

// ---------------------------------------------------------------------------
// Structural invariants — fail on duplicate ids (catalog drift guard)
// ---------------------------------------------------------------------------

test("rule catalog has no duplicate ids", () => {
  const ids = RULE_CATALOG.map((r) => r.id);
  assert.equal(new Set(ids).size, ids.length);
});

test("fix catalog has no duplicate ids", () => {
  const ids = FIX_CATALOG.map((f) => f.id);
  assert.equal(new Set(ids).size, ids.length);
});

test("no duplicate tool names in the surface", () => {
  const caps = buildCapabilities(DEPS);
  const names = caps.tools.map((t) => t.name);
  assert.equal(new Set(names).size, names.length);
});
