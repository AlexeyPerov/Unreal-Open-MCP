import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// P3.6 — gate meta-tool: scoped health check without a preceding mutation.
// Used by agents for manual verification or pre-commit checks. Auto-selects
// rules by extension; the caller narrows via categories / include_rules /
// exclude_rules. Returns the issue list with stable code/severity fields +
// `passed` (false when any Error fired).
//
// Copied from Unity Open MCP's mcp-server/src/tools/validate-edit.ts at copy
// fidelity: the schema, the description, and the route policy are all shared
// contracts (agents that consume one consume the other). The only delta is
// the rule catalog (Unreal's v1 set is broken_soft_references /
// missing_blueprint_parent / compile_errors instead of Unity's
// missing_references / dependencies / scene_prefab_health / asmdef_audit /
// ...).
//
// Route: live (POST /tools/unreal_open_mcp_validate_edit). Read-only.
export const validateEdit: Tool = {
  name: "unreal_open_mcp_validate_edit",
  description:
    "Scoped health check without a preceding mutation. Used by agents for manual verification or pre-commit checks. " +
    "Use include_rules / exclude_rules to narrow the auto-selected rule set. Default (`profile: 'compact'`) returns " +
    "passed + issue counts grouped by severity (no per-issue list); raise to balanced/full for the full issues list, " +
    "and page large result sets with page_size/cursor. Each issue carries ruleId + categoryId (alias), " +
    "severity, code + issueCode (alias), assetPath, description, and fixId + fixSafe when a fix exists. " +
    "Auto-selects rules by extension: .uasset/.umap → broken_soft_references + missing_blueprint_parent + " +
    "compile_errors; .cpp/.h → compile_errors. Error codes: missing_parameter (paths absent), " +
    "invalid_parameter (malformed body), unknown_rule (a requested rule id is not registered — carries " +
    "availableRules for self-correction).",
  inputSchema: {
    type: "object",
    required: ["paths"],
    properties: {
      paths: {
        type: "array",
        items: { type: "string" },
        minItems: 1,
        description:
          "Content paths (/Game/...) or source paths (Source/...) to validate. " +
          "Rules are auto-selected by extension; pass `categories` to override.",
      },
      categories: {
        type: "array",
        items: { type: "string" },
        description: "Verify rule IDs; auto-selected from paths if omitted",
      },
      include_rules: {
        type: "array",
        items: { type: "string" },
        description:
          "Allow-list applied to the resolved rule set. When `categories` is set, include_rules narrows to their intersection; " +
          "without `categories` it is additive on top of the auto-selected set.",
      },
      exclude_rules: {
        type: "array",
        items: { type: "string" },
        description: "Deny-list. Always wins over categories and include_rules.",
      },
      platform_profile: {
        enum: ["mobile", "console", "desktop"],
        default: "desktop",
      },
      profile: {
        enum: ["compact", "balanced", "full"],
        default: "compact",
        description:
          "Token-budget output profile (forward-compat). 'compact' (default) = passed + issue counts grouped by severity " +
          "(issues[] stripped; drill in with balanced/full). 'balanced'/'full' = the full issues list. " +
          "P3.6 ships the compact + full shapes; paging (page_size/cursor) lands with scan_paths.",
      },
      page_size: {
        type: "integer",
        minimum: 1,
        description:
          "Page the issues list (forward-compat; lands with scan_paths). When set, the response carries a `pagination` block " +
          "with a `next_cursor` to resume.",
      },
      cursor: {
        type: "string",
        description:
          "Opaque continuation token from a previous response's `pagination.next_cursor`. Page the issues list (forward-compat).",
      },
      detail: {
        enum: ["summary", "normal", "verbose"],
        default: "normal",
        description:
          "Legacy compression level (alias for `profile`: summary=compact, normal/full=verbose). Prefer `profile`; " +
          "ignored when `profile` is set.",
      },
    },
    additionalProperties: false,
  },
};
