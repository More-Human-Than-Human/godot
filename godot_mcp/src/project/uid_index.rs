use std::collections::HashMap;
use std::path::{Path, PathBuf};

use anyhow::Result;
use walkdir::WalkDir;

use crate::util::abs_to_res;

pub fn build_uid_index(project_path: &Path) -> Result<HashMap<String, String>> {
    let mut index = HashMap::new();
    for entry in WalkDir::new(project_path)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().is_file())
    {
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("uid") {
            continue;
        }
        let uid = std::fs::read_to_string(path)?.trim().to_string();
        if uid.is_empty() {
            continue;
        }
        let source = path.with_extension("");
        if source.exists() {
            let rel = abs_to_res(project_path, &source).unwrap_or_else(|_| {
                source
                    .strip_prefix(project_path)
                    .map(|p| format!("res://{}", p.display()))
                    .unwrap_or_default()
            });
            index.insert(uid, rel);
        }
    }
    Ok(index)
}

pub fn find_orphan_uids(project_path: &Path) -> Result<Vec<String>> {
    let mut orphans = Vec::new();
    for entry in WalkDir::new(project_path)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().is_file())
    {
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("uid") {
            continue;
        }
        let source = path.with_extension("");
        if !source.exists() {
            orphans.push(path.display().to_string());
        }
    }
    Ok(orphans)
}

pub fn find_missing_uids(project_path: &Path) -> Result<Vec<String>> {
    let mut missing = Vec::new();
    for ext in ["gd", "gdshader", "gdextension"] {
        for entry in WalkDir::new(project_path)
            .into_iter()
            .filter_map(|e| e.ok())
            .filter(|e| e.file_type().is_file())
        {
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) != Some(ext) {
                continue;
            }
            let uid_path = path.with_extension(format!("{ext}.uid"));
            if !uid_path.exists() {
                missing.push(path.display().to_string());
            }
        }
    }
    Ok(missing)
}

pub fn uid_path_for(project_path: &Path, res_path: &str) -> PathBuf {
    let rel = res_path.trim_start_matches("res://");
    project_path.join(format!("{rel}.uid"))
}
