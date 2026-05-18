use std::path::Path;

use anyhow::Result;
use regex::Regex;
use serde::Serialize;
use serde_json::{json, Value};

#[derive(Debug, Clone, Serialize)]
pub struct SceneSummary {
    pub format: Option<u32>,
    pub uid: Option<String>,
    pub root_name: Option<String>,
    pub root_type: Option<String>,
    pub root_script: Option<String>,
    pub node_count: usize,
}

#[derive(Debug, Clone, Serialize)]
pub struct SceneTree {
    pub format: Option<u32>,
    pub uid: Option<String>,
    pub root: Value,
    pub ext_resources: Vec<Value>,
}

pub fn parse_scene_summary(path: &Path) -> Result<SceneSummary> {
    let content = read_truncated(path, 64 * 1024)?;
    let header_re = Regex::new(r#"\[gd_scene(?:[^\]]*)?format=(\d+)(?:[^\]]*uid="([^"]*)")?\]"#)?;
    let (format, uid) = if let Some(caps) = header_re.captures(&content) {
        (
            caps.get(1).and_then(|m| m.as_str().parse().ok()),
            caps.get(2).map(|m| m.as_str().to_string()),
        )
    } else {
        (None, None)
    };

    let node_re = Regex::new(r#"\[node name="([^"]+)" type="([^"]+)"(?: parent="([^"]*)")?"#)?;
    let script_re = Regex::new(r#"script = ExtResource\("([^"]+)"\)"#)?;
    let ext_re = Regex::new(r#"\[ext_resource[^\]]*path="([^"]+)"[^\]]*id="([^"]+)"\]"#)?;

    let mut ext_map = std::collections::HashMap::new();
    for caps in ext_re.captures_iter(&content) {
        if let (Some(path), Some(id)) = (caps.get(1), caps.get(2)) {
            ext_map.insert(id.as_str().to_string(), path.as_str().to_string());
        }
    }

    let mut root_name = None;
    let mut root_type = None;
    let mut root_script = None;
    let mut node_count = 0;

    for caps in node_re.captures_iter(&content) {
        node_count += 1;
        let name = caps.get(1).map(|m| m.as_str()).unwrap_or("");
        let node_type = caps.get(2).map(|m| m.as_str()).unwrap_or("");
        let parent = caps.get(3).map(|m| m.as_str());

        if parent.is_none() || parent == Some(".") {
            root_name = Some(name.to_string());
            root_type = Some(node_type.to_string());
        }
    }

    if let Some(caps) = script_re.captures(&content) {
        if let Some(id) = caps.get(1) {
            root_script = ext_map.get(id.as_str()).cloned();
        }
    }

    Ok(SceneSummary {
        format,
        uid,
        root_name,
        root_type,
        root_script,
        node_count,
    })
}

pub fn parse_ext_resources(path: &Path) -> Result<Vec<String>> {
    let content = read_truncated(path, 256 * 1024)?;
    let ext_re = Regex::new(r#"\[ext_resource[^\]]*path="(res://[^"]+)"[^\]]*\]"#)?;
    Ok(ext_re
        .captures_iter(&content)
        .filter_map(|c| c.get(1).map(|m| m.as_str().to_string()))
        .collect())
}

pub fn parse_scene_tree(path: &Path) -> Result<SceneTree> {
    let content = read_truncated(path, 512 * 1024)?;
    let summary = parse_scene_summary(path)?;

    let ext_re =
        Regex::new(r#"\[ext_resource type="([^"]*)"[^\]]*path="([^"]+)"[^\]]*id="([^"]+)"\]"#)?;
    let mut ext_resources = Vec::new();
    for caps in ext_re.captures_iter(&content) {
        ext_resources.push(json!({
            "id": caps.get(3).map(|m| m.as_str()),
            "type": caps.get(1).map(|m| m.as_str()),
            "path": caps.get(2).map(|m| m.as_str()),
        }));
    }

    let node_re = Regex::new(
        r#"\[node name="([^"]+)" type="([^"]+)"(?: parent="([^"]*)")?(?: unique_id=\d+)?\]"#,
    )?;
    let script_re = Regex::new(r#"script = ExtResource\("([^"]+)"\)"#)?;

    struct NodeInfo {
        name: String,
        node_type: String,
        parent: String,
        script_id: Option<String>,
        properties: Vec<(String, String)>,
    }

    let mut nodes: Vec<NodeInfo> = Vec::new();
    let mut current: Option<NodeInfo> = None;

    for line in content.lines() {
        if let Some(caps) = node_re.captures(line) {
            if let Some(n) = current.take() {
                nodes.push(n);
            }
            current = Some(NodeInfo {
                name: caps.get(1).map(|m| m.as_str()).unwrap_or("").to_string(),
                node_type: caps.get(2).map(|m| m.as_str()).unwrap_or("").to_string(),
                parent: caps
                    .get(3)
                    .map(|m| m.as_str().to_string())
                    .unwrap_or_else(|| ".".to_string()),
                script_id: None,
                properties: Vec::new(),
            });
            continue;
        }
        if let Some(ref mut n) = current {
            if let Some(caps) = script_re.captures(line) {
                n.script_id = caps.get(1).map(|m| m.as_str().to_string());
                continue;
            }
            if line.contains('=') && !line.starts_with('[') {
                let trimmed = line.trim();
                if trimmed.starts_with("Packed") && trimmed.len() > 120 {
                    if let Some((k, _)) = trimmed.split_once('=') {
                        n.properties
                            .push((k.trim().to_string(), "<truncated packed array>".to_string()));
                    }
                } else if let Some((k, v)) = trimmed.split_once('=') {
                    let v = v.trim();
                    if v.len() > 200 {
                        n.properties
                            .push((k.trim().to_string(), format!("{}...", &v[..200])));
                    } else {
                        n.properties.push((k.trim().to_string(), v.to_string()));
                    }
                }
            }
        }
    }
    if let Some(n) = current {
        nodes.push(n);
    }

    let ext_map: std::collections::HashMap<String, String> = ext_resources
        .iter()
        .filter_map(|v| {
            Some((
                v["id"].as_str()?.to_string(),
                v["path"].as_str()?.to_string(),
            ))
        })
        .collect();

    fn build_tree(
        nodes: &[NodeInfo],
        ext_map: &std::collections::HashMap<String, String>,
        parent: &str,
    ) -> Vec<Value> {
        nodes
            .iter()
            .filter(|n| n.parent == parent)
            .map(|n| {
                let script = n.script_id.as_ref().and_then(|id| ext_map.get(id)).cloned();
                let props: serde_json::Map<String, Value> = n
                    .properties
                    .iter()
                    .map(|(k, v)| (k.clone(), Value::String(v.clone())))
                    .collect();
                json!({
                    "name": n.name,
                    "type": n.node_type,
                    "script": script,
                    "properties": props,
                    "children": build_tree(nodes, ext_map, &n.name),
                })
            })
            .collect()
    }

    let root_children = build_tree(&nodes, &ext_map, ".");
    let root = if root_children.len() == 1 {
        root_children[0].clone()
    } else {
        json!({ "children": root_children })
    };

    Ok(SceneTree {
        format: summary.format,
        uid: summary.uid,
        root,
        ext_resources,
    })
}

fn read_truncated(path: &Path, max_bytes: usize) -> Result<String> {
    let content = std::fs::read(path)?;
    if content.len() <= max_bytes {
        return Ok(String::from_utf8_lossy(&content).into_owned());
    }
    Ok(String::from_utf8_lossy(&content[..max_bytes]).into_owned())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_main_scene() {
        let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../godot/scenes/Main.tscn");
        let summary = parse_scene_summary(&path).unwrap();
        assert_eq!(summary.root_name.as_deref(), Some("Main"));
        assert_eq!(summary.root_type.as_deref(), Some("Node3D"));
    }
}
