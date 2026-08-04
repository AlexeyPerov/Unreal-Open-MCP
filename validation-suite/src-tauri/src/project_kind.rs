//! Project detection for the Validation Suite.
//!
//! Mirrors the Hub's `project_kind.rs` detection philosophy (cheap,
//! filesystem-only, never shells out), but uses the engine profile's
//! declared markers instead of a hardcoded engine ladder — so a future
//! engine profile can declare its own detection rules without backend
//! changes (idea.md → Multi-engine reuse strategy).
//!
//! For the Unreal profile a folder is valid when it has all of
//! `markers.dirs` and at least one of `markers.files`
//! (`Content/` + `Source/` + `Config/`, and a project marker file like
//! `*.uproject`). A marker file may be a literal relative path
//! (e.g. `Config/DefaultEngine.ini`) or a glob pattern
//! (e.g. `*.uproject`) matched against the project root.

use std::path::Path;

use crate::schemas::{EngineProfile, ProjectCheck};

/// Validate a candidate project folder against an engine profile's
/// markers. Never panics — returns a `ProjectCheck` with a clear,
/// human-readable reason on rejection so the project bar can show
/// actionable copy (phase-1 task 3: reject non-matching folders with
/// a clear error).
pub fn check_project(path: &Path, profile: &EngineProfile) -> ProjectCheck {
    let path_str = path.to_string_lossy().to_string();
    if !path.is_dir() {
        return ProjectCheck {
            valid: false,
            path: path_str.clone(),
            reason: Some(format!(
                "Not a directory: {path_str}. Pick the {display} project root folder.",
                display = profile.display_name
            )),
        };
    }
    // All declared dirs must exist.
    for d in &profile.markers.dirs {
        if !path.join(d).is_dir() {
            return ProjectCheck {
                valid: false,
                path: path_str,
                reason: Some(format!(
                    "Not a {display} project: missing required folder \"{d}\".",
                    display = profile.display_name
                )),
            };
        }
    }
    // At least one declared marker file must exist. A marker may be a
    // literal relative path or a glob pattern (e.g. `*.uproject`).
    if !profile.markers.files.is_empty() {
        let any = profile
            .markers
            .files
            .iter()
            .any(|f| marker_file_exists(path, f));
        if !any {
            return ProjectCheck {
                valid: false,
                path: path_str,
                reason: Some(format!(
                    "Not a {display} project: none of the marker files ({files}) were found.",
                    display = profile.display_name,
                    files = profile.markers.files.join(", ")
                )),
            };
        }
    }
    ProjectCheck {
        valid: true,
        path: path_str,
        reason: None,
    }
}

/// True when the marker `pattern` resolves to an existing file under
/// `root`. A pattern containing a `*` wildcard is treated as a glob
/// matched against the entries reachable from `pattern`'s parent dir;
/// a plain path is checked directly. Only the final path segment may
/// contain a wildcard (e.g. `*.uproject`), which covers the Unreal
/// project-descriptor detection case.
fn marker_file_exists(root: &Path, pattern: &str) -> bool {
    if !pattern.contains('*') {
        return root.join(pattern).is_file();
    }
    // Split into a literal parent dir (no wildcards) + a glob tail.
    let mut literal_dir = root.to_path_buf();
    let mut tail: Option<&str> = None;
    for seg in pattern.split('/') {
        if seg.contains('*') {
            tail = Some(seg);
            break;
        }
        literal_dir = literal_dir.join(seg);
    }
    let glob_tail = match tail {
        Some(g) => g,
        None => return false,
    };
    let entries = match std::fs::read_dir(&literal_dir) {
        Ok(e) => e,
        Err(_) => return false,
    };
    for entry in entries.flatten() {
        let name = entry.file_name();
        let name_str = name.to_string_lossy();
        if glob_matches(glob_tail, &name_str) && entry.path().is_file() {
            return true;
        }
    }
    false
}

/// A minimal single-segment glob matcher supporting `*` (any run of
/// chars). Sufficient for `*.uproject`-style marker patterns.
fn glob_matches(pattern: &str, name: &str) -> bool {
    if !pattern.contains('*') {
        return pattern == name;
    }
    let (prefix, suffix) = match pattern.split_once('*') {
        Some((p, s)) => (p, s),
        None => return false,
    };
    name.starts_with(prefix) && name.ends_with(suffix) && name.len() >= prefix.len() + suffix.len()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::schemas::{ProfilePaths, ProjectMarkers};

    fn unreal_profile() -> EngineProfile {
        EngineProfile {
            id: "unreal".to_string(),
            display_name: "Unreal Open MCP".to_string(),
            mcp_cli_binary: "unreal-open-mcp".to_string(),
            paths: ProfilePaths {
                fixture_root: "Content/_ValidationSuite/<test-id>/".to_string(),
                state_root: "Saved/ValidationSuite/".to_string(),
                state_file: "Saved/ValidationSuite/.state.json".to_string(),
                actuals_dir: "Saved/ValidationSuite/actuals/".to_string(),
                exports_dir: "Saved/ValidationSuite/exports/".to_string(),
            },
            markers: ProjectMarkers {
                dirs: vec!["Content".to_string(), "Source".to_string(), "Config".to_string()],
                files: vec!["*.uproject".to_string()],
            },
            companions: vec![],
            placeholders: vec!["{fixtureRoot}".to_string(), "{projectRoot}".to_string()],
            tool_name_prefix: "unreal_open_mcp_".to_string(),
        }
    }

    fn mkdir(p: &Path) {
        std::fs::create_dir_all(p).unwrap();
    }
    fn touch(p: &Path) {
        if let Some(parent) = p.parent() {
            mkdir(parent);
        }
        std::fs::write(p, b"x").unwrap();
    }

    #[test]
    fn valid_unreal_project_passes() {
        let dir = tempfile::tempdir().unwrap();
        mkdir(&dir.path().join("Content"));
        mkdir(&dir.path().join("Source"));
        mkdir(&dir.path().join("Config"));
        touch(&dir.path().join("Demo.uproject"));
        let check = check_project(dir.path(), &unreal_profile());
        assert!(check.valid);
        assert!(check.reason.is_none());
    }

    #[test]
    fn missing_content_dir_rejected_with_reason() {
        let dir = tempfile::tempdir().unwrap();
        mkdir(&dir.path().join("Source"));
        mkdir(&dir.path().join("Config"));
        touch(&dir.path().join("Demo.uproject"));
        let check = check_project(dir.path(), &unreal_profile());
        assert!(!check.valid);
        assert!(check.reason.unwrap().contains("Content"));
    }

    #[test]
    fn missing_uproject_marker_rejected_with_reason() {
        let dir = tempfile::tempdir().unwrap();
        mkdir(&dir.path().join("Content"));
        mkdir(&dir.path().join("Source"));
        mkdir(&dir.path().join("Config"));
        let check = check_project(dir.path(), &unreal_profile());
        assert!(!check.valid);
        assert!(check.reason.unwrap().contains("marker files"));
    }

    #[test]
    fn non_directory_rejected() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("notafolder");
        std::fs::write(&file, b"x").unwrap();
        let check = check_project(&file, &unreal_profile());
        assert!(!check.valid);
        assert!(check.reason.unwrap().contains("Not a directory"));
    }

    #[test]
    fn glob_marker_matches_any_uproject_name() {
        let dir = tempfile::tempdir().unwrap();
        mkdir(&dir.path().join("Content"));
        mkdir(&dir.path().join("Source"));
        mkdir(&dir.path().join("Config"));
        touch(&dir.path().join("UnrealOpenMcpDemo.uproject"));
        let check = check_project(dir.path(), &unreal_profile());
        assert!(check.valid);
    }

    #[test]
    fn glob_matches_helper() {
        assert!(glob_matches("*.uproject", "Demo.uproject"));
        assert!(glob_matches("*.uproject", "X.uproject"));
        assert!(!glob_matches("*.uproject", "Demo.unity"));
        assert!(!glob_matches("*.uproject", "uproject"));
        // Exact name when no wildcard.
        assert!(glob_matches("Demo.uproject", "Demo.uproject"));
    }
}
