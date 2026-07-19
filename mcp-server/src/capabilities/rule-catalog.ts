// Capability-discovery rule catalog.
//
// Static metadata for verify rules and fix providers. This is the documented
// capability surface agents discover via `unreal_open_mcp_capabilities`. The
// catalog is versioned with the package: `implemented` flags reflect what
// ships in the matching bridge/verify package release.
//
// Implemented-rule metadata mirrors the C++ verify package issue mappers
// (packages/verify/Source/UnrealOpenMcpVerify/Private/Rules/*) and the fix
// registry (packages/verify/Source/UnrealOpenMcpVerify/Private/Fixes/
// FixProviderRegistry.cpp). The issue codes link rules to fixes — a fix
// provider's `CanFix` matches on the issue key the rule emitted.
//
// P3.8 ships the v1 catalog: three implemented rules
// (broken_soft_references / missing_blueprint_parents / compile_errors) and
// one implemented Safe fix (clear_broken_soft_reference). The Planned surface
// lists the verify rules still on the roadmap (content_path_hygiene) so an
// agent asking for capabilities learns the shape of upcoming rules without
// discovering gaps by trial.
//
// Maintenance rule: when a new verify rule or fix provider ships in
// packages/verify, this catalog MUST be updated in the same task
// (packages/verify/AGENTS.md §Capability catalog sync). The catalog drift
// tests in build-capabilities.test.ts fail on any C++/TS rule/fix drift.

export type CapabilityStatus = "implemented" | "planned";

export interface RuleIssueDescriptor {
  /** Issue code emitted by the rule (e.g. `broken_soft_reference`). */
  code: string;
  /** Default severity (`Error` | `Warning`). */
  severity: "Error" | "Warning";
  /** Fix IDs that can resolve this issue code, if any. */
  fixIds: string[];
  /** True when the issue is only emitted during a full project scan. */
  fullScanOnly?: boolean;
  /**
   * Machine-readable root-cause code (e.g. `missing_soft_reference`,
   * `missing_blueprint_class`). Stable across releases — agents branch on it.
   * Mirrors the rootCause taxonomy used by the C++ rule issue mappers so the
   * scan_paths / validate_edit response and this catalog classify findings
   * identically.
   */
  rootCause?: string;
  /**
   * Short, user-visible remediation playbook for this issue class. Clean of
   * internal IDs (no milestone / spec / execution-plan references).
   */
  remediation?: string;
}

export interface RuleCapability {
  id: string;
  title: string;
  description: string;
  /** Asset kinds the rule analyzes (e.g. `blueprint`, `level`, `source`). */
  applicableAssetKinds: string[];
  /** File extensions the rule applies to, when known. */
  applicableExtensions?: string[];
  implemented: boolean;
  status: CapabilityStatus;
  /** Issue codes this rule can emit. Empty for planned rules. */
  issues: RuleIssueDescriptor[];
  /** Guidance shown to the agent when the rule is not implemented. */
  guidance?: string;
}

export interface FixCapability {
  id: string;
  implemented: boolean;
  status: CapabilityStatus;
  /** Rule IDs this fix can resolve issues for. */
  rules: string[];
  /** Issue codes this fix addresses. */
  issueCodes: string[];
  /** True when the fix is safe to auto-apply (no destructive side effects). */
  safe: boolean;
  /** Guidance shown to the agent when the fix is not implemented. */
  guidance?: string;
}

// ---------------------------------------------------------------------------
// Implemented rules — mirror packages/verify/Source/UnrealOpenMcpVerify/
// Private/Rules/{BrokenSoftReferences,MissingBlueprintParent,CompileErrors}/
// Issue*Codes.h. Rule IDs are plural (Unity convention); issue codes are
// singular so a single finding reads naturally.
// ---------------------------------------------------------------------------

const BROKEN_SOFT_REFERENCES_ISSUES: RuleIssueDescriptor[] = [
  {
    code: "broken_soft_reference",
    severity: "Error",
    fixIds: ["clear_broken_soft_reference"],
    rootCause: "missing_soft_reference",
    remediation:
      "A soft object pointer (FSoftObjectPath) resolves to a target that the " +
      "Asset Registry cannot load — the referenced asset was deleted, moved, " +
      "or never saved. Clear the broken pointer via apply_fix " +
      "(clear_broken_soft_reference) when the issue pins a precise top-level " +
      "property path, or repoint it to the correct target.",
  },
];

const MISSING_BLUEPRINT_PARENT_ISSUES: RuleIssueDescriptor[] = [
  {
    code: "missing_blueprint_parent",
    severity: "Error",
    fixIds: [],
    rootCause: "missing_blueprint_class",
    remediation:
      "A Blueprint's parent class fails to resolve — the parent was renamed, " +
      "removed, or its module is no longer compiled into the editor. Restore " +
      "the parent class (re-add the C++ class or repoint the Blueprint to a " +
      "valid parent via the Blueprint Editor's Class Settings). No Safe " +
      "automated fix exists today; clearing a missing parent is rarely safe.",
  },
];

const COMPILE_ERRORS_ISSUES: RuleIssueDescriptor[] = [
  {
    code: "compile_error",
    severity: "Error",
    fixIds: [],
    rootCause: "build_blocker",
    remediation:
      "A C++ / Live Coding compile failed and left the editor in a hot-reload-" +
      "broken state. Open the source file pinned by the diagnostic and fix the " +
      "compile error (the bridge never auto-compiles; the rule is read-only " +
      "by design). Re-run Live Coding once the source compiles cleanly.",
  },
];

// ---------------------------------------------------------------------------
// Planned rules — the verify-rule roadmap. Mirrors the stub rule families
// listed in specs/roadmap.md and packages/verify/AGENTS.md. Each carries
// actionable guidance so an agent gets a structured "not yet available"
// signal instead of discovering the gap by trial.
// ---------------------------------------------------------------------------

const PLANNED_RULES: RuleCapability[] = [
  {
    id: "content_path_hygiene",
    title: "Content path hygiene",
    description:
      "Flags malformed content paths, missing package metadata, and naming " +
      "convention violations under /Game/.",
    applicableAssetKinds: ["asset"],
    applicableExtensions: [".uasset", ".umap"],
    implemented: false,
    status: "planned",
    issues: [],
    guidance:
      "Not yet ported. Inspect content paths via the asset registry " +
      "(asset_find / asset_list) until the rule ships.",
  },
];

// ---------------------------------------------------------------------------
// Full catalog
// ---------------------------------------------------------------------------

export const RULE_CATALOG: RuleCapability[] = [
  {
    id: "broken_soft_references",
    title: "Broken soft references",
    description:
      "Walks FSoftObjectPath properties (top-level and depth-1 struct-nested) " +
      "and asks the Asset Registry whether each target resolves. Emits " +
      "broken_soft_reference per unresolved pointer with a " +
      "<targetPackage>:<propertyPath> suffix so apply_fix can identify the " +
      "exact pointer to clear. Applicable to any asset that can hold a soft " +
      "object pointer (Blueprints, DataAssets, levels).",
    applicableAssetKinds: ["blueprint", "data_asset", "level"],
    applicableExtensions: [".uasset", ".umap"],
    implemented: true,
    status: "implemented",
    issues: BROKEN_SOFT_REFERENCES_ISSUES,
  },
  {
    id: "missing_blueprint_parents",
    title: "Missing Blueprint parents",
    description:
      "Reads UBlueprint::ParentClass and asks the resolver whether the parent " +
      "path resolves. Emits missing_blueprint_parent per Blueprint whose " +
      "parent fails to load (renamed, removed, or module not compiled). The " +
      "Unreal analogue of Unity/Godot 'missing script' — a Blueprint whose " +
      "parent class no longer resolves.",
    applicableAssetKinds: ["blueprint"],
    applicableExtensions: [".uasset"],
    implemented: true,
    status: "implemented",
    issues: MISSING_BLUEPRINT_PARENT_ISSUES,
  },
  {
    id: "compile_errors",
    title: "Compile errors",
    description:
      "Asks the compile-status provider for the current Live Coding state and " +
      "structured diagnostics. Never calls Compile() — the rule bans compile " +
      "side effects. A Failed state with no per-file diagnostics emits a " +
      "coarse (project) finding so a known failure is never silently " +
      "swallowed. Applicable to C++ source paths; auto-selected for .cpp/.h " +
      "extensions.",
    applicableAssetKinds: ["source"],
    applicableExtensions: [".cpp", ".h"],
    implemented: true,
    status: "implemented",
    issues: COMPILE_ERRORS_ISSUES,
  },
  ...PLANNED_RULES,
];

// ---------------------------------------------------------------------------
// Fix capability entries.
//
// v1 ships one Safe fix provider: clear_broken_soft_reference (nulls a broken
// FSoftObjectProperty the broken_soft_references rule pinned, then saves the
// package). More providers land in later phases; each addition MUST update
// this catalog in the same task (see packages/verify/AGENTS.md).
//
// Safe-flag policy (mirrors FFixProviderRegistry::TryResolveSafe in the C++
// verify package): the catalog value matches the provider's Describe().bSafe
// at registration time. The registry re-derives Safe from Describe() per
// call so a provider that flips Safe based on issue specifics (e.g.
// struct-nested soft pointers refuse with safe:false) is honored even when
// the catalog lists the default as Safe.
// ---------------------------------------------------------------------------

export const FIX_CATALOG: FixCapability[] = [
  {
    id: "clear_broken_soft_reference",
    implemented: true,
    status: "implemented",
    rules: ["broken_soft_references"],
    issueCodes: ["broken_soft_reference"],
    // Nulls an already-broken pointer at a precise top-level property path
    // and saves the package. The pointer resolved to nothing to begin with,
    // so clearing it loses no asset data. Struct-nested findings return
    // safe:false from Describe() at apply time so the gate never auto-
    // suggests them in v1.
    safe: true,
  },
];

export function implementedRules(): RuleCapability[] {
  return RULE_CATALOG.filter((r) => r.implemented);
}

export function plannedRules(): RuleCapability[] {
  return RULE_CATALOG.filter((r) => !r.implemented);
}

export function implementedFixes(): FixCapability[] {
  return FIX_CATALOG.filter((f) => f.implemented);
}
