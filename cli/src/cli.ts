// CLI dispatcher.
//
// Adapted from Unity Open MCP's mcp-server/src/cli/cli.ts. This module is the
// entry logic for `unreal-open-mcp-cli <command>`. It:
//   1. parses argv (src/args.ts)
//   2. short-circuits --help / --version on the light fast path
//   3. dispatches to a command module when one is implemented
//   4. prints output and returns the exit code
//
// The light fast-path deps (parseCliArgs from args.ts, the helpText/versionText
// formatters from help-text.ts, writeAndDrain, and readPackageVersion) stay
// static so `--version` / `--help` never touch a heavier import graph. When the
// first real command lands (P8.2+), its module is loaded via a dynamic
// `import()` that runs only when that subcommand is dispatched — mirroring
// Unity's lazy-load contract.
//
// P8.2: `install-plugin` is the first implemented command. Every other
// recognized-but-unimplemented command returns a structured
// `command_not_implemented` message and exit code 2 so an agent or script gets
// a clean signal (never a silent no-op or a hang).

import { parseCliArgs, IMPLEMENTED_COMMANDS } from "./args.js";
import { helpText, versionText } from "./help-text.js";
import { readPackageVersion } from "./package-version.js";
import { BIN_NAME } from "./constants.js";

export interface CliRunOptions {
  /** Package version, used by --version. Falls back to readPackageVersion(). */
  version?: string;
  /** Invocation name for help text (default: unreal-open-mcp-cli). */
  binName?: string;
  /** argv after the node binary + script path. Defaults to process.argv.slice(2). */
  argv?: string[];
}

export interface CliRunOutcome {
  /** True when argv was handled (a command, --help, --version, or an error). */
  handled: boolean;
  /** Process exit code; only meaningful when handled === true. */
  exitCode: number;
}

/**
 * Run the CLI. Writes to stdout/stderr itself and is meant to be the top of the
 * process. It does NOT call process.exit — the caller does, so tests can drive
 * it without tearing down the test runner.
 */
export async function runCli(opts: CliRunOptions = {}): Promise<CliRunOutcome> {
  const argv = opts.argv ?? process.argv.slice(2);
  const parsed = parseCliArgs(argv);
  const binName = opts.binName ?? BIN_NAME;

  // --help / --version short-circuit before any project-path requirement AND
  // before any heavier module is imported. Every stdout/stderr write goes
  // through writeAndDrain so the caller's process.exit() can't truncate output
  // that's still in the pipe buffer.
  if (parsed.command === "help") {
    await writeAndDrain(process.stdout, helpText(binName) + "\n");
    return { handled: true, exitCode: 0 };
  }
  if (parsed.command === "version") {
    const version = opts.version || readPackageVersion();
    await writeAndDrain(process.stdout, versionText(version, binName) + "\n");
    return { handled: true, exitCode: 0 };
  }

  if (parsed.error) {
    await writeAndDrain(
      process.stderr,
      `${binName}: ${parsed.error}\n\n${helpText(binName)}\n`,
    );
    return { handled: true, exitCode: 2 };
  }

  // No command → print help (the CLI has no server-fallthrough mode; unlike
  // Unity's unified bin, this package is CLI-only).
  if (!parsed.command) {
    await writeAndDrain(process.stdout, helpText(binName) + "\n");
    return { handled: true, exitCode: 0 };
  }

  // Recognized command. Unimplemented commands surface a clean not-implemented
  // signal; implemented commands (IMPLEMENTED_COMMANDS) dynamically import their
  // module so the `--version` / `--help` fast path never pulls in the heavier
  // command graph (mirrors Unity Open MCP's lazy-load contract).
  if (!IMPLEMENTED_COMMANDS.includes(parsed.command)) {
    const msg = `'${parsed.command}' is recognized but not implemented yet in this build. ` +
      `Implemented: ${IMPLEMENTED_COMMANDS.length === 0 ? "(none yet)" : IMPLEMENTED_COMMANDS.join(", ")}.`;
    await writeAndDrain(process.stderr, `${binName}: ${msg}\n`);
    return { handled: true, exitCode: 2 };
  }

  // --- dispatch implemented commands -------------------------------------
  // The first positional after the command token is the `[projectDir]` arg for
  // the commands that accept it (install-plugin today).
  const positionalProjectDir = parsed.positionals[0];

  if (parsed.command === "install-plugin") {
    const mod = await import("./commands/install-plugin.js");
    const outcome = await mod.runInstallPluginCommand(
      {
        projectPath: parsed.projectPath,
        pluginSource: parsed.pluginSource,
        symlink: parsed.symlink,
        withVerify: parsed.withVerify,
        dryRun: parsed.dryRun,
        json: parsed.json,
        positionalProjectDir,
      },
      (s) => writeAndDrain(process.stdout, s),
      (s) => writeAndDrain(process.stderr, s),
      binName,
    );
    return { handled: true, exitCode: outcome.exitCode };
  }

  if (parsed.command === "setup-mcp") {
    const mod = await import("./commands/setup-mcp.js");
    const outcome = await mod.runSetupMcpCommand(
      {
        projectPath: parsed.projectPath,
        port: parsed.port,
        serverCommand: parsed.serverCommand,
        dryRun: parsed.dryRun,
        list: parsed.list,
        json: parsed.json,
        positionalAgent: positionalProjectDir,
      },
      (s) => writeAndDrain(process.stdout, s),
      (s) => writeAndDrain(process.stderr, s),
      binName,
    );
    return { handled: true, exitCode: outcome.exitCode };
  }

  if (parsed.command === "open") {
    const mod = await import("./commands/open.js");
    const outcome = await mod.runOpenCommand(
      {
        projectPath: parsed.projectPath,
        engineRoot: parsed.engineRoot,
        noBuild: parsed.noBuild,
        port: parsed.port,
        json: parsed.json,
        positionalProjectDir,
      },
      (s) => writeAndDrain(process.stdout, s),
      (s) => writeAndDrain(process.stderr, s),
      binName,
    );
    return { handled: true, exitCode: outcome.exitCode };
  }

  if (parsed.command === "wait-for-ready") {
    const mod = await import("./commands/wait-for-ready.js");
    const outcome = await mod.runWaitForReadyCommand(
      {
        projectPath: parsed.projectPath,
        port: parsed.port,
        timeout: parsed.timeout,
        interval: parsed.interval,
        json: parsed.json,
        positionalProjectDir,
      },
      (s) => writeAndDrain(process.stdout, s),
      (s) => writeAndDrain(process.stderr, s),
      binName,
    );
    return { handled: true, exitCode: outcome.exitCode };
  }

  if (parsed.command === "status") {
    const mod = await import("./commands/status.js");
    const outcome = await mod.runStatusCommand(
      {
        projectPath: parsed.projectPath,
        port: parsed.port,
        noProbe: parsed.noProbe,
        json: parsed.json,
        positionalProjectDir,
      },
      (s) => writeAndDrain(process.stdout, s),
      (s) => writeAndDrain(process.stderr, s),
      binName,
    );
    return { handled: true, exitCode: outcome.exitCode };
  }

  if (parsed.command === "configure") {
    const mod = await import("./commands/configure.js");
    const outcome = await mod.runConfigureCommand(
      {
        projectPath: parsed.projectPath,
        bridgePort: parsed.bridgePort,
        clearBridgePort: parsed.clearBridgePort,
        dryRun: parsed.dryRun,
        json: parsed.json,
        positionalProjectDir,
      },
      (s) => writeAndDrain(process.stdout, s),
      (s) => writeAndDrain(process.stderr, s),
      binName,
    );
    return { handled: true, exitCode: outcome.exitCode };
  }

  // Future command dispatch goes here. Kept as an unreachable guard so the
  // compiler agrees the function always returns.
  return { handled: false, exitCode: 0 };
}

/**
 * Minimal writable interface {@link writeAndDrain} needs. Narrowed from
 * NodeJS.WriteStream so the function is unit-testable with a small fake
 * (a real stream's `.write` returns false under backpressure and the callback
 * form fires only after the OS-level write completes).
 */
export interface DrainableWritable {
  write(chunk: string): boolean;
  write(chunk: string, callback: (err?: Error | null) => void): boolean;
  once(event: "drain", listener: () => void): unknown;
}

/**
 * Write a string to a Writable stream and resolve once the OS has consumed it.
 *
 * `stream.write` is asynchronous when the destination is a pipe. The
 * `write(chunk, callback)` overload is the correct primitive — its callback
 * fires only after libuv completes (or errors) the actual write to the kernel,
 * regardless of backpressure. Resolves immediately for TTY/file destinations
 * where the write is effectively synchronous.
 */
export function writeAndDrain(
  stream: DrainableWritable,
  chunk: string,
): Promise<void> {
  return new Promise((resolve, reject) => {
    stream.write(chunk, (err) => {
      if (err) reject(err);
      else resolve();
    });
  });
}
