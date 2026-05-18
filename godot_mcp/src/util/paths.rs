use std::path::{Path, PathBuf};

use anyhow::{anyhow, Result};

pub fn path_to_uri(path: &Path) -> String {
    let abs = path.canonicalize().unwrap_or_else(|_| path.to_path_buf());
    let mut normalized = abs.display().to_string().replace('\\', "/");
    if normalized.starts_with("//") {
        normalized = normalized.trim_start_matches('/').to_string();
    }
    format!("file:///{normalized}")
}

pub fn resolve_res_path(project_path: &Path, file_path: &str) -> Result<PathBuf> {
    if file_path.starts_with("res://") {
        let rel = file_path.trim_start_matches("res://");
        return Ok(project_path.join(rel));
    }

    let path = PathBuf::from(file_path);
    if path.is_absolute() {
        return Ok(path);
    }

    Ok(project_path.join(path))
}

pub fn res_to_abs(project_path: &Path, res_path: &str) -> Result<PathBuf> {
    if !res_path.starts_with("res://") {
        return Err(anyhow!("not a res:// path: {res_path}"));
    }
    Ok(project_path.join(res_path.trim_start_matches("res://")))
}

pub fn abs_to_res(project_path: &Path, abs: &Path) -> Result<String> {
    let canonical_project = project_path
        .canonicalize()
        .map_err(|e| anyhow!("project path: {e}"))?;
    let canonical_abs = abs.canonicalize().map_err(|e| anyhow!("file path: {e}"))?;
    let rel = canonical_abs
        .strip_prefix(&canonical_project)
        .map_err(|_| anyhow!("path not under project root"))?;
    Ok(format!(
        "res://{}",
        rel.display().to_string().replace('\\', "/")
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn path_to_uri_formats_absolute_paths() {
        let uri = path_to_uri(Path::new("/tmp/example.gd"));
        assert!(uri.starts_with("file:///"));
        assert!(uri.ends_with("/tmp/example.gd"));
    }
}
