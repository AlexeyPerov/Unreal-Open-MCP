#!/usr/bin/env node

// Entry point for `unreal-open-mcp-cli`.
//
// Thin launcher. The CLI is its own publishable package (ADR-007 — the stdio
// MCP server ships under the separate `unreal-open-mcp` bin). This entry keeps
// only the light CLI + version helpers static; when the first real command
// module lands (P8.2+), it is loaded dynamically only on the matching branch.
//
// Mirrors Unity Open MCP's mcp-server/src/index.ts shape, minus the stdio
// server fallthrough (this package has no server mode).

import { runCli } from "./cli.js";
import { readPackageVersion } from "./package-version.js";

// Read the version from package.json at runtime so the maintainer version-sync
// flow keeps the reported CLI version current without editing this source file.
const PACKAGE_VERSION = readPackageVersion();

async function main(): Promise<void> {
  const outcome = await runCli({ version: PACKAGE_VERSION });
  if (outcome.handled) {
    process.exit(outcome.exitCode);
  }
  // runCli only returns handled:false from the (future) command-dispatch guard;
  // in P8.1 every path is handled. If we ever reach here, treat it as success.
  process.exit(0);
}

main().catch((err) => {
  console.error("unreal-open-mcp-cli fatal:", err);
  process.exit(1);
});
