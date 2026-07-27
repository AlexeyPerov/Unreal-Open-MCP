import test from "node:test";
import assert from "node:assert/strict";
import { sourceRead } from "./source-read.js";
import { sourceList } from "./source-list.js";
import { sourceCreateClass } from "./source-create-class.js";
import { sourceUpdate } from "./source-update.js";
import { sourceDelete } from "./source-delete.js";
import { ALL_TOOLS } from "./index.js";

// P7.1 + P7.2 acceptance: the source read/list family is read-only (no
// paths_hint / gate), while the create/update/delete family is mutating
// (paths_hint required + gate enforce/warn/off). The catalog surface advertised
// via tools/list must expose all five under the unreal_open_mcp_ prefix and
// document the jail + structured error codes each tool surfaces.

// Shared schema shape helpers.
type ToolSchema = {
  type: string;
  required?: string[];
  properties: Record<
    string,
    {
      type?: string;
      enum?: string[];
      items?: { type?: string };
      default?: string | boolean;
      minimum?: number;
    }
  >;
  additionalProperties: boolean;
};

const mutatingGateProps = (schema: ToolSchema) => {
  assert.ok(schema.properties.paths_hint, "mutator must expose paths_hint");
  assert.equal(schema.properties.paths_hint.type, "array");
  assert.equal(schema.properties.paths_hint.items?.type, "string");
  assert.ok(schema.properties.gate, "mutator must expose gate");
  assert.deepEqual(schema.properties.gate.enum, ["enforce", "warn", "off"]);
  assert.equal(schema.properties.gate.default, "enforce");
  assert.ok(
    schema.required?.includes("paths_hint"),
    "mutator must list paths_hint in required",
  );
};

test("source read/list tools are registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(sourceRead.name, "unreal_open_mcp_source_read");
  assert.equal(sourceList.name, "unreal_open_mcp_source_list");
  for (const n of ["unreal_open_mcp_source_read", "unreal_open_mcp_source_list"]) {
    assert.ok(ALL_TOOLS.some((t) => t.name === n), `${n} registered`);
  }
});

test("source_read requires path and advertises the line slice + max_lines + jail", () => {
  const schema = sourceRead.inputSchema as unknown as ToolSchema;
  assert.deepEqual(schema.required, ["path"]);
  assert.ok(schema.properties.path, "path arg");
  assert.ok(schema.properties.start_line, "start_line arg");
  assert.ok(schema.properties.end_line, "end_line arg");
  assert.ok(schema.properties.max_lines, "max_lines arg");
  assert.equal(schema.additionalProperties, false);
  const desc = sourceRead.description ?? "";
  assert.match(desc, /read-only/i);
  assert.match(desc, /jail/i, "documents the Source/ jail");
  assert.match(desc, /path_escapes_jail/, "documents the escape error code");
  assert.match(desc, /truncated/, "documents truncation");
  assert.match(desc, /lines:\[/, "documents the lines result shape");
  // No gate surface on a read-only tool.
  assert.ok(!("paths_hint" in schema.properties), "no paths_hint on read-only tool");
  assert.ok(!("gate" in schema.properties), "no gate on read-only tool");
});

test("source_list advertises the module / recursive / extensions surface", () => {
  const schema = sourceList.inputSchema as unknown as ToolSchema;
  assert.ok(schema.properties.module, "module arg");
  assert.ok(schema.properties.recursive, "recursive arg");
  assert.ok(schema.properties.extensions, "extensions arg");
  assert.equal(schema.additionalProperties, false);
  const desc = sourceList.description ?? "";
  assert.match(desc, /read-only/i);
  assert.match(desc, /jail/i, "documents the Source/ jail");
  assert.match(desc, /path_escapes_jail/, "documents the escape error code");
  assert.match(desc, /module_not_found/, "documents the module_not_found code");
  assert.match(desc, /files:\[/, "documents the files result shape");
  assert.match(desc, /total_bytes/, "documents total_bytes");
  assert.match(desc, /extensions/i, "documents the extension allow-list");
  // No required args (module/recursive/extensions all optional).
  assert.ok(!("required" in schema) || schema.required === undefined);
  // No gate surface on a read-only tool.
  assert.ok(!("paths_hint" in schema.properties), "no paths_hint on read-only tool");
  assert.ok(!("gate" in schema.properties), "no gate on read-only tool");
});

test("source_create_class is registered + mutating (paths_hint required + gate)", () => {
  assert.equal(sourceCreateClass.name, "unreal_open_mcp_source_create_class");
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_source_create_class"),
    "registered",
  );
  const schema = sourceCreateClass.inputSchema as unknown as ToolSchema;
  assert.equal(schema.type, "object");
  assert.ok(schema.required?.includes("class_name"), "class_name required");
  mutatingGateProps(schema);
  assert.equal(schema.additionalProperties, false);
  // parent_class enum — the four supported kinds.
  assert.deepEqual(
    schema.properties.parent_class?.enum,
    ["UObject", "Actor", "ActorComponent", "None"],
  );
  assert.equal(schema.properties.parent_class?.default, "UObject");
  assert.equal(schema.properties.force?.default, false);
  // Description documents the jail + the full structured error list + the
  // derived-prefix contract + the no-auto-compile contract.
  const desc = sourceCreateClass.description ?? "";
  assert.match(desc, /mutating/i);
  assert.match(desc, /jail/i);
  assert.match(desc, /path_escapes_jail/);
  assert.match(desc, /already_exists/);
  assert.match(desc, /module_not_found/);
  assert.match(desc, /write_failed/);
  assert.match(desc, /invalid_parameter/);
  assert.match(desc, /U\/A\/F prefix/);
  assert.match(desc, /force/i);
  // No auto-compile — points at the P7.3 surface.
  assert.match(desc, /source_compile|does not compile|does NOT compile/i);
});

test("source_update is registered + mutating (paths_hint required + gate)", () => {
  assert.equal(sourceUpdate.name, "unreal_open_mcp_source_update");
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_source_update"),
    "registered",
  );
  const schema = sourceUpdate.inputSchema as unknown as ToolSchema;
  assert.equal(schema.type, "object");
  assert.ok(schema.required?.includes("path"), "path required");
  assert.ok(schema.required?.includes("content"), "content required");
  mutatingGateProps(schema);
  assert.ok(schema.properties.start_line, "start_line arg");
  assert.ok(schema.properties.end_line, "end_line arg");
  assert.equal(schema.properties.start_line?.minimum, 1);
  assert.equal(schema.properties.end_line?.minimum, 1);
  assert.equal(schema.additionalProperties, false);
  const desc = sourceUpdate.description ?? "";
  assert.match(desc, /mutating/i);
  assert.match(desc, /jail/i);
  assert.match(desc, /path_escapes_jail/);
  assert.match(desc, /file_not_found/);
  assert.match(desc, /invalid_line_range/);
  assert.match(desc, /write_failed/);
  // The EOL-preservation splice contract.
  assert.match(desc, /CRLF/i);
  // Full-file vs range mode documented.
  assert.match(desc, /full/i);
  assert.match(desc, /range/i);
});

test("source_delete is registered + mutating + destructive (paths_hint required + gate)", () => {
  assert.equal(sourceDelete.name, "unreal_open_mcp_source_delete");
  assert.ok(
    ALL_TOOLS.some((t) => t.name === "unreal_open_mcp_source_delete"),
    "registered",
  );
  const schema = sourceDelete.inputSchema as unknown as ToolSchema;
  assert.equal(schema.type, "object");
  assert.ok(schema.required?.includes("path"), "path required");
  mutatingGateProps(schema);
  assert.equal(schema.additionalProperties, false);
  const desc = sourceDelete.description ?? "";
  assert.match(desc, /mutating/i);
  assert.match(desc, /destructive/i);
  assert.match(desc, /jail/i);
  assert.match(desc, /path_escapes_jail/);
  assert.match(desc, /file_not_found/);
  assert.match(desc, /is_directory/);
  assert.match(desc, /delete_failed/);
  assert.match(desc, /invalid_parameter/);
  // The not-undoable warning — a destructive tool must say so.
  assert.match(desc, /not undoable|NOT undoable/i);
});
