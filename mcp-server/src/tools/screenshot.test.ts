import test from "node:test";
import assert from "node:assert/strict";
import { screenshotViewport } from "./screenshot-viewport.js";
import { screenshotGameView } from "./screenshot-game-view.js";
import { screenshotCamera } from "./screenshot-camera.js";
import { screenshotIsolated } from "./screenshot-isolated.js";
import { ALL_TOOLS } from "./index.js";

// P5.5 — the four-tool screenshot family. Pins the catalog surface advertised
// via tools/list: each tool is registered under the `unreal_open_mcp_` prefix,
// is read-only (no gate / paths_hint), returns a PNG as MCP image content, and
// advertises the shared dimension caps + the per-tool required args.

const FOUR = [screenshotViewport, screenshotGameView, screenshotCamera, screenshotIsolated];

test("screenshot tools are registered under the unreal_open_mcp_ prefix", () => {
  for (const tool of FOUR) {
    assert.ok(
      tool.name.startsWith("unreal_open_mcp_screenshot_"),
      `${tool.name} must start with unreal_open_mcp_screenshot_`,
    );
    assert.ok(ALL_TOOLS.some((t) => t.name === tool.name), `${tool.name} must be in ALL_TOOLS`);
  }
});

test("screenshot tools are read-only (no gate / paths_hint surface)", () => {
  // Read-only tools expose neither `paths_hint` nor `gate` — those are the
  // mutating-tool surfaces. Their absence is the contract an agent parses to
  // know no gate applies.
  for (const tool of FOUR) {
    const props = (tool.inputSchema as { properties: Record<string, unknown> }).properties;
    assert.ok(!("paths_hint" in props), `${tool.name} must not expose paths_hint`);
    assert.ok(!("gate" in props), `${tool.name} must not expose gate`);
  }
});

test("screenshot tools document the image-content return + error codes", () => {
  for (const tool of FOUR) {
    const desc = tool.description ?? "";
    assert.ok(desc.length > 0, `${tool.name}: description present`);
    assert.match(desc, /image content block/i, `${tool.name}: must document image return`);
    assert.match(desc, /read-only/i, `${tool.name}: must document read-only`);
    assert.match(desc, /\[1, 2048\]/, `${tool.name}: must document the dimension cap`);
    assert.match(desc, /image_too_large/, `${tool.name}: must document image_too_large`);
  }
});

test("screenshot_viewport + screenshot_game_view take optional width/height only", () => {
  for (const tool of [screenshotViewport, screenshotGameView]) {
    const schema = tool.inputSchema as {
      required?: string[];
      properties: Record<string, { type: string }>;
    };
    assert.deepEqual(schema.required ?? [], [], `${tool.name}: no required args`);
    assert.equal(schema.properties.width?.type, "integer");
    assert.equal(schema.properties.height?.type, "integer");
  }
});

test("screenshot_game_view description names the pie_not_running error", () => {
  // The PIE precondition is the load-bearing contract for game-view — pin it.
  assert.match(screenshotGameView.description ?? "", /pie_not_running/);
});

test("screenshot_camera requires `camera` and exposes width/height/fov", () => {
  const schema = screenshotCamera.inputSchema as {
    required: string[];
    properties: Record<string, { type: string }>;
  };
  assert.deepEqual(schema.required, ["camera"]);
  assert.equal(schema.properties.camera.type, "string");
  assert.equal(schema.properties.width?.type, "integer");
  assert.equal(schema.properties.height?.type, "integer");
  assert.equal(schema.properties.fov?.type, "number");
});

test("screenshot_isolated requires `actor` and exposes width/height/background/fov", () => {
  const schema = screenshotIsolated.inputSchema as {
    required: string[];
    properties: Record<string, { type: string }>;
  };
  assert.deepEqual(schema.required, ["actor"]);
  assert.equal(schema.properties.actor.type, "string");
  assert.equal(schema.properties.background?.type, "string");
  assert.equal(schema.properties.fov?.type, "number");
});
