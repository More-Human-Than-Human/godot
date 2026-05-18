use std::path::Path;

use anyhow::Result;
use regex::Regex;

#[derive(Debug, Clone)]
pub struct ScriptMeta {
    pub class_name: Option<String>,
    pub extends: Option<String>,
    pub is_tool: bool,
}

pub fn parse_script_meta(path: &Path) -> Result<ScriptMeta> {
    let content = std::fs::read_to_string(path)?;
    let class_re = Regex::new(r#"^class_name\s+(\w+)"#)?;
    let extends_re = Regex::new(r#"^extends\s+(\S+)"#)?;
    let tool_re = Regex::new(r#"^@tool\b"#)?;

    let mut class_name = None;
    let mut extends = None;
    let mut is_tool = false;

    for line in content.lines() {
        if let Some(caps) = class_re.captures(line) {
            class_name = caps.get(1).map(|m| m.as_str().to_string());
        }
        if let Some(caps) = extends_re.captures(line) {
            extends = caps.get(1).map(|m| m.as_str().to_string());
        }
        if tool_re.is_match(line) {
            is_tool = true;
        }
    }

    Ok(ScriptMeta {
        class_name,
        extends,
        is_tool,
    })
}

pub fn extract_res_refs(content: &str) -> Vec<String> {
    let re = Regex::new(r#"(?:preload|load)\("(res://[^"]+)"\)"#).unwrap();
    re.captures_iter(content)
        .filter_map(|c| c.get(1).map(|m| m.as_str().to_string()))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_main_script() {
        let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../godot/scripts/Main.gd");
        let meta = parse_script_meta(&path).unwrap();
        assert_eq!(meta.extends.as_deref(), Some("Node3D"));
    }
}
