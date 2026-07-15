#!/usr/bin/env python3
"""check-editor-boundary.py — Editor/Runtime boundary CI guard.

Invariant (docs/architecture.md, "Editor / Runtime boundary"):
    Editor code may reference Runtime code; Runtime code may NEVER reference
    Editor code.

This script enforces the *surface* of the Runtime module that ModuleRules
cannot catch on its own — `#include` of editor-only headers in Runtime
sources and editor-only module names in the Runtime Build.cs dependency
arrays. It is advisory to ModuleRules, not a replacement: both must hold.

What it checks (deterministic, no fuzzy text):
  * .h/.cpp/.hpp under UnrealOpenMcpRuntime/ — `#include` tokens that resolve
    to editor-only headers (umbrella basenames like UnrealEd.h / Editor.h, or
    editor module include roots like "UnrealEd/", "Editor/", "AssetTools/").
  * UnrealOpenMcpRuntime.Build.cs — editor-only module names inside
    Public/PrivateDependencyModuleNames (UnrealEd, Slate, AssetTools, ...).

Suppression (auditable, never silent):
  Add a comment carrying the marker WITH a justification on the offending
  line or the line immediately above it:

      #include "UnrealEd.h"  // unreal-open-mcp:allow-editor-boundary: <why>
      // unreal-open-mcp:allow-editor-boundary: <why>
      "Slate",   // Build.cs — editor UI deliberately pulled into Runtime

  A marker without a justification is itself a violation (forces review).

Exit code: 0 on a clean boundary, 1 on any violation.

Usage:
    python scripts/check-editor-boundary.py            # scan the repo
    python scripts/check-editor-boundary.py --self-test  # verify the detector
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import List, Tuple

# ---------------------------------------------------------------------------
# Policy: what counts as "editor-only".
#
# Keep these lists explicit and readable — they ARE the policy and must be
# reviewable in a PR. Start strict (broad) and narrow only when a justified
# false positive lands; never use fuzzy substring text (see safeguard in the
# P1.8 plan). Add entries here as new editor leaks surface.
# ---------------------------------------------------------------------------

# Include path fragments. A violation fires when an `#include` token STARTS
# WITH one of these roots. These are module include roots (trailing slash) so
# a runtime header that merely shares a word never false-triggers.
EDITOR_INCLUDE_PREFIXES: Tuple[str, ...] = (
    "UnrealEd/",
    "LevelEditor/",
    "AssetTools/",
    "ContentBrowser/",
    "PropertyEditor/",
    "DetailCustomizations/",
    "KismetCompiler/",
    "BlueprintGraph/",
    "MaterialEditor/",
    "Editor/",                       # umbrella editor include root (no runtime headers live here)
    "Kismet2/",                      # Blueprint editor runtime
    "Subsystems/EditorSubsystem",    # editor subsystem base + derivatives (GameInstanceSubsystem stays allowed)
)

# Exact header basenames that are editor-only umbrellas. Matched against the
# basename of the include token so neither path depth nor quote/angle style
# matters. Do NOT add shared names that also exist in runtime modules.
EDITOR_INCLUDE_BASENAMES = frozenset(
    {
        "UnrealEd.h",
        "Editor.h",
        "LevelEditor.h",
        "AssetToolsModule.h",
        "AssetRegistryModule.h",
        "EditorSubsystem.h",
        "MainFrame.h",
        "HotReload.h",
        "SourceCodeNavigation.h",
    }
)

# Module names that must NEVER appear in the Runtime module's
# Public/PrivateDependencyModuleNames. They pull editor-only symbols and break
# packaging when linked into a runtime module. Policy (not just capability):
# even modules with runtime-capable builds (Slate/SlateCore) are denied here
# to keep the Runtime module lean — relax via a justified suppression if a
# future runtime feature genuinely requires them.
EDITOR_DEPENDENCY_MODULES = frozenset(
    {
        "UnrealEd",
        "Slate",
        "SlateCore",
        "EditorStyle",
        "AppFramework",
        "AssetTools",
        "AssetRegistry",
        "ContentBrowser",
        "PropertyEditor",
        "DetailCustomizations",
        "KismetCompiler",
        "BlueprintGraph",
        "MaterialEditor",
        "UnrealEdUtilities",
        "EditorSubsystem",
        "MainFrame",
        "LevelEditor",
        "SourceCodeNavigation",
        "HotReload",
        "LiveCoding",
        "GraphEditor",
        "WorkspaceMenuStructure",
    }
)

# Source extensions we scan for C/C++ includes.
CPP_SOURCE_EXTS = frozenset({".h", ".hpp", ".cpp", ".cc", ".cxx"})
BUILD_CS_NAME = "UnrealOpenMcpRuntime.Build.cs"
RUNTIME_MODULE_DIR_DEFAULT = "packages/bridge/Source/UnrealOpenMcpRuntime"

# Suppression marker — must carry a justification after the colon.
SUPPRESSION_MARKER = "unreal-open-mcp:allow-editor-boundary"
_SUPPRESSION_RE = re.compile(
    re.escape(SUPPRESSION_MARKER) + r"\s*:\s*(?P<reason>\S.*?)\s*$"
)
_MARKER_ALONE_RE = re.compile(re.escape(SUPPRESSION_MARKER))


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------


def _rel(path: Path, root: Path) -> str:
    try:
        return os.path.relpath(path, root)
    except ValueError:
        return str(path)


def _strip_comments(text: str) -> str:
    """Remove // line comments and /* */ block comments from C# Build.cs text.

    Module names are simple identifiers, never contain // or /*, so stripping
    inside strings is not a concern here. Lets us extract quoted module names
    without tripping on explanatory comments that mention editor modules.
    """
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def _suppression_for(lines: List[str], idx: int) -> Tuple[str, str]:
    """Classify the suppression around line index ``idx`` (0-based).

    Returns one of:
      ("valid", reason)   — a justified marker suppresses a violation here
      ("invalid", "")     — marker present but WITHOUT a justification
      ("none", "")        — no marker on this line or the previous line
    """
    candidates = []
    if 0 <= idx < len(lines):
        candidates.append(lines[idx])
    if idx - 1 >= 0:
        candidates.append(lines[idx - 1])

    invalid_seen = False
    for line in candidates:
        m = _SUPPRESSION_RE.search(line)
        if m and m.group("reason").strip():
            # A justified marker wins; prefer it over a bare marker nearby.
            return ("valid", m.group("reason").strip())
        if _MARKER_ALONE_RE.search(line):
            # Marker seen but no justified reason -> record; keep scanning the
            # other candidate in case a valid one is nearby.
            invalid_seen = True
    if invalid_seen:
        return ("invalid", "")
    return ("none", "")


# ---------------------------------------------------------------------------
# Detectors
# ---------------------------------------------------------------------------

_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]')
_STRING_LITERAL_RE = re.compile(r'"([A-Za-z0-9_.]+)"')


def find_include_violations(file_path: Path) -> List[Tuple[int, str, str]]:
    """Return [(line_no, include_token, message)] for editor-only includes."""
    violations: List[Tuple[int, str, str]] = []
    try:
        raw = file_path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        raw = file_path.read_text(encoding="utf-8", errors="replace")
    lines = raw.splitlines()

    for i, line in enumerate(lines):
        m = _INCLUDE_RE.match(line)
        if not m:
            continue
        header = m.group(1).strip()
        base = os.path.basename(header)
        is_editor = header.startswith(EDITOR_INCLUDE_PREFIXES) or base in EDITOR_INCLUDE_BASENAMES
        if not is_editor:
            continue

        status, _reason = _suppression_for(lines, i)
        if status == "valid":
            continue
        if status == "invalid":
            violations.append(
                (
                    i + 1,
                    header,
                    f"editor include '{header}' suppressed without justification; "
                    f"add a reason after the marker: '{SUPPRESSION_MARKER}: <why>'",
                )
            )
            continue
        violations.append(
            (i + 1, header, f"Runtime module must not include editor-only header '{header}'")
        )
    return violations


def find_dependency_violations(file_path: Path) -> List[Tuple[int, str, str]]:
    """Return [(line_no, module_name, message)] for editor-only Build.cs deps."""
    violations: List[Tuple[int, str, str]] = []
    raw = file_path.read_text(encoding="utf-8")
    stripped = _strip_comments(raw)
    lines = stripped.splitlines()

    for i, line in enumerate(lines):
        for m in _STRING_LITERAL_RE.finditer(line):
            module = m.group(1)
            if module not in EDITOR_DEPENDENCY_MODULES:
                continue
            status, _reason = _suppression_for(lines, i)
            if status == "valid":
                continue
            if status == "invalid":
                violations.append(
                    (
                        i + 1,
                        module,
                        f"dependency '{module}' suppressed without justification; "
                        f"add a reason after the marker: '{SUPPRESSION_MARKER}: <why>'",
                    )
                )
                continue
            violations.append(
                (
                    i + 1,
                    module,
                    f"Runtime module must not depend on editor-only module '{module}'",
                )
            )
    return violations


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------


def iter_target_files(runtime_dir: Path):
    for path in sorted(runtime_dir.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix in CPP_SOURCE_EXTS:
            yield path, "cpp"
        elif path.name == BUILD_CS_NAME:
            yield path, "buildcs"


def run_scan(runtime_dir: Path, repo_root: Path) -> int:
    if not runtime_dir.is_dir():
        print(
            f"error: runtime module dir not found: {_rel(runtime_dir, repo_root)}",
            file=sys.stderr,
        )
        return 2

    files_scanned = 0
    violations: List[Tuple[str, int, str]] = []

    for file_path, kind in iter_target_files(runtime_dir):
        files_scanned += 1
        if kind == "cpp":
            found = find_include_violations(file_path)
        else:
            found = find_dependency_violations(file_path)
        rel = _rel(file_path, repo_root)
        for line_no, _token, message in found:
            violations.append((rel, line_no, message))

    for rel, line_no, message in violations:
        print(f"{rel}:{line_no}: error: {message}", file=sys.stderr)

    if violations:
        print(
            f"\nEditor/Runtime boundary check FAILED: {len(violations)} violation(s) "
            f"across {files_scanned} file(s). Editor code may reference Runtime; "
            f"Runtime must NEVER reference Editor. "
            f"(see {SUPPRESSION_MARKER}: <reason> to suppress with review)",
            file=sys.stderr,
        )
        return 1

    print(
        f"Editor/Runtime boundary check passed: {files_scanned} Runtime file(s) clean.",
        file=sys.stderr,
    )
    return 0


# ---------------------------------------------------------------------------
# Self-test: prove the detector fires and that suppression works. Runs fully
# in-memory against embedded snippets (satisfies the optional P1.8 negative
# fixture + "rule set is deterministic" acceptance criterion).
# ---------------------------------------------------------------------------


def self_test() -> int:
    failures: List[str] = []

    cpp_bad = (
        '#include "CoreMinimal.h"\n'
        '#include "UnrealEd.h"\n'                       # editor umbrella
        '#include "AssetTools/AssetToolsModule.h"\n'   # editor root
        '#include "Subsystems/EditorSubsystem.h"\n'    # editor subsystem base
        '#include "HAL/PlatformProcess.h"\n'           # legit runtime
    )
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        (d / "Bad.h").write_text(cpp_bad, encoding="utf-8")
        v = find_include_violations(d / "Bad.h")
        bad_lines = {line_no for line_no, _tok, _msg in v}
        # Expect lines 2, 3, 4 flagged; line 1 + 5 must stay clean.
        if not {2, 3, 4}.issubset(bad_lines):
            failures.append(f"include detector missed editor headers (got lines {sorted(bad_lines)})")
        if 1 in bad_lines or 5 in bad_lines:
            failures.append(f"include detector false-positived on runtime headers (got {sorted(bad_lines)})")

    # Suppression with justification is honored; bare marker is flagged.
    cpp_supp = (
        '#include "UnrealEd.h"  // unreal-open-mcp:allow-editor-boundary: needed for X\n'
    )
    cpp_supp_bare = '#include "UnrealEd.h"  // unreal-open-mcp:allow-editor-boundary\n'
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        (d / "Supp.h").write_text(cpp_supp, encoding="utf-8")
        if find_include_violations(d / "Supp.h"):
            failures.append("justified suppression was not honored")
        (d / "SuppBare.h").write_text(cpp_supp_bare, encoding="utf-8")
        if not find_include_violations(d / "SuppBare.h"):
            failures.append("bare suppression marker (no reason) was not flagged")

    # Build.cs dependency detection ignores comment prose but catches deps.
    build_cs = (
        'using UnrealBuildTool;\n'
        'public class UnrealOpenMcpRuntime : ModuleRules\n'
        '{\n'
        '  public UnrealOpenMcpRuntime(ReadOnlyTargetRules Target) : base(Target)\n'
        '  {\n'
        '    // Keep this module free of UnrealEd / Slate — do not add them.\n'
        '    PublicDependencyModuleNames.AddRange(new string[]\n'
        '    {\n'
        '      "Core",\n'
        '    });\n'
        '    PrivateDependencyModuleNames.AddRange(new string[]\n'
        '    {\n'
        '      "Slate",\n'        # editor-only dep (by policy)
        '      "Networking",\n'   # legit runtime
        '    });\n'
        '  }\n'
        '}\n'
    )
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        f = d / BUILD_CS_NAME
        f.write_text(build_cs, encoding="utf-8")
        v = find_dependency_violations(f)
        mods = {tok for _ln, tok, _msg in v}
        if "Slate" not in mods:
            failures.append(f"Build.cs detector missed editor dep (got {sorted(mods)})")
        if "Core" in mods or "Networking" in mods:
            failures.append(f"Build.cs detector false-positived on runtime deps (got {sorted(mods)})")

    if failures:
        print("self-test FAILED:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("self-test passed.", file=sys.stderr)
    return 0


def main(argv: List[str]) -> int:
    here = Path(__file__).resolve().parent
    default_repo_root = here.parent

    p = argparse.ArgumentParser(
        description="Editor/Runtime boundary CI guard for the UnrealOpenMcpRuntime module.",
    )
    p.add_argument(
        "--runtime-dir",
        default=str(default_repo_root / RUNTIME_MODULE_DIR_DEFAULT),
        help=f"Runtime module dir to scan (default: {RUNTIME_MODULE_DIR_DEFAULT}).",
    )
    p.add_argument(
        "--repo-root",
        default=str(default_repo_root),
        help="Repo root for relative diagnostics (default: script's parent's parent).",
    )
    p.add_argument(
        "--self-test",
        action="store_true",
        help="Run the in-memory detector self-test and exit (no repo scan).",
    )
    args = p.parse_args(argv)

    if args.self_test:
        return self_test()

    return run_scan(Path(args.runtime_dir), Path(args.repo_root))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
