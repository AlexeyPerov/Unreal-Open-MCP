import test from "node:test";
import assert from "node:assert/strict";
import { sourceRead } from "./source-read.js";
import { sourceList } from "./source-list.js";
import { ALL_TOOLS } from "./index.js";

// P7.1 acceptance: the source read/list family is registered under the
// `unreal_open_mcp_` prefix, source_read requires `path` + advertises the line
// slice + max_lines + jail contract, and source_list advertises the module /
// recursive / extensions surface. Both are read-only (no paths_hint / gate).

test("source tools are registered under the unreal_open_mcp_ prefix", () => {
  assert.equal(sourceRead.name, "unreal_open_mcp_source_read");
  assert.equal(sourceList.name, "unreal_open_mcp_source_list");
  for (const n of ["unreal_open_mcp_source_read", "unreal_open_mcp_source_list"]) {
    assert.ok(ALL_TOOLS.some((t) => t.name === n), `${n} registered`);
  }
});

test("source_read requires path and advertises the line slice + max_lines + jail", () => {
  const schema = sourceRead.inputSchema as unknown as {
    required?: string[];
    properties: Record<string, unknown>;
    additionalProperties: boolean;
  };
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
  const schema = sourceList.inputSchema as unknown as {
    properties: Record<string, unknown>;
    additionalProperties: boolean;
  };
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
  assert.ok(!("required" in schema) || (schema as { required?: string[] }).required === undefined);
  // No gate surface on a read-only tool.
  assert.ok(!("paths_hint" in schema.properties), "no paths_hint on read-only tool");
  assert.ok(!("gate" in schema.properties), "no gate on read-only tool");
});
