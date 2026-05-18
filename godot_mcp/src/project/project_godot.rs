use std::path::Path;

use anyhow::Result;
use regex::Regex;
use serde::Serialize;

#[derive(Debug, Clone, Serialize)]
pub struct ProjectGodot {
    pub name: String,
    pub main_scene: Option<String>,
    pub features: Vec<String>,
    pub icon: Option<String>,
    pub physics_engine: Option<String>,
    pub editor_plugins: Vec<String>,
    pub input_actions: Vec<String>,
}

pub fn parse(path: &Path) -> Result<ProjectGodot> {
    let content = std::fs::read_to_string(path)?;
    let mut section = String::new();
    let mut name = String::from("Unknown");
    let mut main_scene = None;
    let mut features = Vec::new();
    let mut icon = None;
    let mut physics_engine = None;
    let mut editor_plugins = Vec::new();
    let mut input_actions = Vec::new();

    let kv_re = Regex::new(r#"^([^=]+)=(.*)$"#)?;

    for line in content.lines() {
        let line = line.trim();
        if line.starts_with(';') || line.is_empty() {
            continue;
        }
        if line.starts_with('[') && line.ends_with(']') {
            section = line[1..line.len() - 1].to_string();
            if section == "input" && !line.contains('=') {
                // input action header like `move_forward={`
            }
            continue;
        }

        if section == "input" {
            if line.ends_with('{') && !line.contains('=') {
                let action = line.trim_end_matches('{').trim().to_string();
                if !action.is_empty() {
                    input_actions.push(action);
                }
                continue;
            }
            if line.ends_with('{') {
                let action = line.split('=').next().unwrap_or("").trim().to_string();
                if !action.is_empty() {
                    input_actions.push(action);
                }
                continue;
            }
        }

        if let Some(caps) = kv_re.captures(line) {
            let key = caps.get(1).map(|m| m.as_str()).unwrap_or("").trim();
            let value = caps.get(2).map(|m| m.as_str()).unwrap_or("").trim();

            match (section.as_str(), key) {
                ("application", "config/name") => {
                    name = value.trim_matches('"').to_string();
                }
                ("application", "run/main_scene") => {
                    main_scene = Some(value.trim_matches('"').to_string());
                }
                ("application", "config/features") => {
                    features = parse_packed_string_array(value);
                }
                ("application", "config/icon") => {
                    icon = Some(value.trim_matches('"').to_string());
                }
                ("physics", "3d/physics_engine") => {
                    physics_engine = Some(value.trim_matches('"').to_string());
                }
                ("editor_plugins", "enabled") => {
                    editor_plugins = parse_packed_string_array(value);
                }
                _ => {}
            }
        }
    }

    Ok(ProjectGodot {
        name,
        main_scene,
        features,
        icon,
        physics_engine,
        editor_plugins,
        input_actions,
    })
}

fn parse_packed_string_array(value: &str) -> Vec<String> {
    if !value.starts_with("PackedStringArray(") {
        return Vec::new();
    }
    let inner = value
        .trim_start_matches("PackedStringArray(")
        .trim_end_matches(')');
    inner
        .split(',')
        .map(|s| s.trim().trim_matches('"').to_string())
        .filter(|s| !s.is_empty())
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_desert_game_project() {
        let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../godot/project.godot");
        let pg = parse(&path).unwrap();
        assert_eq!(pg.name, "Desert Game");
        assert!(pg.input_actions.contains(&"move_forward".to_string()));
    }
}
