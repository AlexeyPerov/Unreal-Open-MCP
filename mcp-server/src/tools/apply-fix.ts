import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P3.7 — apply_fix: apply a verify rule fix action.
//
// Copied from Unity Open MCP's mcp-server/src/tools/apply-fix.ts at copy
// fidelity: the schema, the description, and the route policy are all shared
// contracts (agents that consume one consume the other). The only delta is
// the implemented-fixes roster — Unreal's v1 set is the single Safe provider
// `clear_broken_soft_reference` (nulls a broken soft object pointer at a
// precise top-level property path); more providers land in later phases.
//
// Route: live (POST /tools/unreal_open_mcp_apply_fix). Mutating (default gate
// Enforce). The dispatcher short-circuits dry_run:true directly to the inner
// handler (no gate around a preview that mutates nothing); non-dry-run applies
// route through the bridge's ApplyFixGateRunner so a FixRollback snapshot
// protects the asset.
export const applyFix: Tool = {
  name: "unreal_open_mcp_apply_fix",
  description:
    "Apply a verify rule fix action. Supports dry_run (default true) to preview the fix before applying. " +
    "Returns the gate envelope when dry_run is false (dry_run short-circuits the gate entirely). " +
    "Implemented fixes: clear_broken_soft_reference (safe — nulls a broken soft object pointer at a " +
    "precise top-level property path; refuses struct-nested properties in v1). " +
    "Safe auto-fix rollback: a non-dry-run apply that fails or introduces new errors under enforce is " +
    "restored to its pre-fix state and the response carries a top-level `rollback` block " +
    "({rolledBack, reason, restoredPaths}). Non-dry-run applies are gate-runner-mediated: a FixRollback " +
    "snapshot is active so a corrupting fix is automatically reverted on failure or new errors. " +
    "Use gate: \"off\" ONLY when you accept that the fix commits with no rollback — the response then " +
    "carries rollbackDisabled: true to flag that no automatic restore is available.",
  inputSchema: {
    type: "object",
    required: ["issue_id"],
    properties: {
      fix_id: {
        type: "string",
        description:
          "Fix action id from issue payload (e.g. clear_broken_soft_reference). " +
          "If omitted, the response lists every fix that can resolve the given issue_id.",
      },
      issue_id: {
        type: "string",
        description:
          "Issue key from validate_edit or scan_paths (format: ruleId|severity|assetPath|issueCode). " +
          "Severity is case-insensitive and accepts any of error/warn/warning — copy the key verbatim " +
          "from a scan_paths issue's ruleId|severity|assetPath|issueCode fields. The asset-path " +
          "component is auto-derived as the gate's paths_hint when paths_hint is omitted.",
      },
      dry_run: {
        type: "boolean",
        default: true,
        description:
          "Preview the fix without applying. Default true. dry_run skips the gate entirely.",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description: "Gate mode when dry_run is false. Ignored for dry_run. Non-dry-run applies are " +
          "always gate-runner-mediated (rollback snapshot active). Use gate: \"off\" only when you " +
          "accept the fix commits with no rollback — the response then carries rollbackDisabled: true.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Optional mutation scope override. Auto-derived from issue_id's asset path when omitted.",
      },
    },
    additionalProperties: false,
  },
};
