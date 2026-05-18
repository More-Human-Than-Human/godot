pub mod gdscript;
pub mod project_godot;
pub mod tscn;
pub mod uid_index;

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};

use anyhow::Result;
use serde_json::{json, Value};
use walkdir::WalkDir;

use crate::util::abs_to_res;

#[derive(Clone)]
pub struct ProjectIndex {
    pub project_path: PathBuf,
}

impl ProjectIndex {
    pub fn new(project_path: PathBuf) -> Self {
        Self { project_path }
    }

    pub fn info(&self) -> Result<Value> {
        let pg = project_godot::parse(&self.project_path.join("project.godot"))?;
        Ok(json!({
            "project_path": self.project_path,
            "name": pg.name,
            "main_scene": pg.main_scene,
            "features": pg.features,
            "icon": pg.icon,
            "physics_engine": pg.physics_engine,
            "editor_plugins": pg.editor_plugins,
        }))
    }

    pub fn list_scenes(&self) -> Result<Value> {
        let mut scenes = Vec::new();
        for entry in WalkDir::new(&self.project_path)
            .into_iter()
            .filter_map(|e| e.ok())
            .filter(|e| e.file_type().is_file())
        {
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) != Some("tscn") {
                continue;
            }
            let rel = abs_to_res(&self.project_path, path).unwrap_or_else(|_| {
                path.strip_prefix(&self.project_path)
                    .map(|p| format!("res://{}", p.display()))
                    .unwrap_or_default()
            });
            let summary = tscn::parse_scene_summary(path)?;
            scenes.push(json!({
                "path": rel,
                "format": summary.format,
                "uid": summary.uid,
                "root_name": summary.root_name,
                "root_type": summary.root_type,
                "root_script": summary.root_script,
                "node_count": summary.node_count,
            }));
        }
        scenes.sort_by(|a, b| {
            a["path"]
                .as_str()
                .unwrap_or("")
                .cmp(b["path"].as_str().unwrap_or(""))
        });
        Ok(json!({ "count": scenes.len(), "scenes": scenes }))
    }

    pub fn list_scripts(&self) -> Result<Value> {
        let mut scripts = Vec::new();
        for entry in WalkDir::new(&self.project_path)
            .into_iter()
            .filter_map(|e| e.ok())
            .filter(|e| e.file_type().is_file())
        {
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) != Some("gd") {
                continue;
            }
            let rel = abs_to_res(&self.project_path, path).unwrap_or_default();
            let meta = gdscript::parse_script_meta(path)?;
            scripts.push(json!({
                "path": rel,
                "class_name": meta.class_name,
                "extends": meta.extends,
                "is_tool": meta.is_tool,
            }));
        }
        scripts.sort_by(|a, b| {
            a["path"]
                .as_str()
                .unwrap_or("")
                .cmp(b["path"].as_str().unwrap_or(""))
        });
        Ok(json!({ "count": scripts.len(), "scripts": scripts }))
    }

    pub fn scene_tree(&self, file_path: &str) -> Result<Value> {
        let abs = crate::util::resolve_res_path(&self.project_path, file_path)?;
        let tree = tscn::parse_scene_tree(&abs)?;
        Ok(json!({
            "path": file_path,
            "format": tree.format,
            "uid": tree.uid,
            "root": tree.root,
            "ext_resources": tree.ext_resources,
        }))
    }

    pub fn dependencies(&self) -> Result<Value> {
        let cache_path = self.project_path.join(".godot/editor/filesystem_cache10");
        if cache_path.exists() {
            if let Ok(deps) = parse_filesystem_cache(&cache_path) {
                return Ok(json!({ "source": "filesystem_cache", "dependencies": deps }));
            }
        }

        let mut graph: HashMap<String, Vec<String>> = HashMap::new();
        for entry in WalkDir::new(&self.project_path)
            .into_iter()
            .filter_map(|e| e.ok())
            .filter(|e| e.file_type().is_file())
        {
            let path = entry.path();
            let ext = path.extension().and_then(|e| e.to_str()).unwrap_or("");
            if !matches!(ext, "tscn" | "gd" | "gdextension" | "godot") {
                continue;
            }
            let Ok(rel) = abs_to_res(&self.project_path, path) else {
                continue;
            };
            let refs = collect_res_refs(path, ext)?;
            if !refs.is_empty() {
                let mut sorted: Vec<_> = refs.into_iter().collect();
                sorted.sort();
                graph.insert(rel, sorted);
            }
        }
        Ok(json!({ "source": "scan", "dependencies": graph }))
    }

    pub fn class_registry(&self) -> Result<Value> {
        let mut classes: HashMap<String, String> = HashMap::new();

        for entry in WalkDir::new(&self.project_path)
            .into_iter()
            .filter_map(|e| e.ok())
            .filter(|e| e.file_type().is_file())
        {
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) != Some("gd") {
                continue;
            }
            if let Ok(meta) = gdscript::parse_script_meta(path) {
                if let Some(cn) = meta.class_name {
                    let rel = abs_to_res(&self.project_path, path).unwrap_or_default();
                    classes.insert(cn, rel);
                }
            }
        }

        let cache_path = self
            .project_path
            .join(".godot/global_script_class_cache.cfg");
        if cache_path.exists() {
            if let Ok(cache_classes) = parse_class_cache(&cache_path) {
                for (name, path) in cache_classes {
                    classes.entry(name).or_insert(path);
                }
            }
        }

        Ok(json!({ "count": classes.len(), "classes": classes }))
    }

    pub fn input_map(&self) -> Result<Value> {
        let pg = project_godot::parse(&self.project_path.join("project.godot"))?;
        Ok(json!({ "actions": pg.input_actions }))
    }

    pub fn uid_index(&self) -> Result<Value> {
        let index = uid_index::build_uid_index(&self.project_path)?;
        Ok(json!({
            "count": index.len(),
            "uids": index,
        }))
    }
}

fn collect_res_refs(path: &Path, ext: &str) -> Result<HashSet<String>> {
    let content = std::fs::read_to_string(path)?;
    let mut refs = HashSet::new();
    let re = regex::Regex::new(r#"res://[^"\')\s]+"#)?;
    for cap in re.captures_iter(&content) {
        if let Some(m) = cap.get(0) {
            refs.insert(m.as_str().to_string());
        }
    }
    if ext == "tscn" {
        if let Ok(summary) = tscn::parse_ext_resources(path) {
            refs.extend(summary);
        }
    }
    Ok(refs)
}

fn parse_filesystem_cache(path: &Path) -> Result<HashMap<String, Vec<String>>> {
    let content = std::fs::read_to_string(path)?;
    let mut graph = HashMap::new();
    let re = regex::Regex::new(r#"^(\S+\.(?:tscn|gd|gdshader|gdextension))\::.*::(.*)$"#)?;
    for line in content.lines() {
        if let Some(caps) = re.captures(line) {
            let file = caps.get(1).map(|m| m.as_str()).unwrap_or("");
            let deps_str = caps.get(2).map(|m| m.as_str()).unwrap_or("");
            if deps_str.contains("::") {
                let res_path = deps_str.split("::").last().unwrap_or("").trim();
                if res_path.starts_with("res://") {
                    graph
                        .entry(format!("res://{}", file.trim_start_matches("res://")))
                        .or_insert_with(Vec::new)
                        .push(res_path.to_string());
                }
            }
        }
    }
    for deps in graph.values_mut() {
        deps.sort();
        deps.dedup();
    }
    Ok(graph)
}

fn parse_class_cache(path: &Path) -> Result<HashMap<String, String>> {
    let content = std::fs::read_to_string(path)?;
    let mut classes = HashMap::new();
    let re = regex::Regex::new(r#"path="(res://[^"]+)""#)?;
    let name_re = regex::Regex::new(r#"class="([^"]+)""#)?;
    let mut current_name = None;
    for line in content.lines() {
        if let Some(caps) = name_re.captures(line) {
            current_name = caps.get(1).map(|m| m.as_str().to_string());
        }
        if let Some(caps) = re.captures(line) {
            if let Some(path) = caps.get(1).map(|m| m.as_str().to_string()) {
                if let Some(name) = current_name.take() {
                    classes.insert(name, path);
                }
            }
        }
    }
    Ok(classes)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn project_path() -> PathBuf {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../godot")
    }

    #[test]
    fn project_info_parses() {
        let idx = ProjectIndex::new(project_path());
        let info = idx.info().unwrap();
        assert_eq!(info["name"], "Desert Game");
        assert!(info["main_scene"]
            .as_str()
            .unwrap()
            .contains("LoadingScreen"));
    }

    #[test]
    fn list_scenes_finds_main() {
        let idx = ProjectIndex::new(project_path());
        let scenes = idx.list_scenes().unwrap();
        assert!(scenes["count"].as_u64().unwrap() > 0);
    }
}
