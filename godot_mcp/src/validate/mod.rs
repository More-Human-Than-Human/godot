use std::collections::HashSet;
use std::path::PathBuf;

use anyhow::Result;
use regex::Regex;
use serde_json::{json, Value};
use walkdir::WalkDir;

use crate::project::{gdscript, project_godot, tscn, uid_index};
use crate::util::res_to_abs;

#[derive(Debug, Clone, serde::Serialize)]
pub struct ValidationIssue {
    pub severity: String,
    pub message: String,
    pub file: Option<String>,
    pub line: Option<u32>,
    pub resource: Option<String>,
}

#[derive(Clone)]
pub struct Validator {
    project_path: PathBuf,
}

impl Validator {
    pub fn new(project_path: PathBuf) -> Self {
        Self { project_path }
    }

    pub fn validate_resources(&self) -> Result<Value> {
        let issues = self.collect_resource_issues()?;
        let errors: Vec<_> = issues.iter().filter(|i| i.severity == "error").collect();
        let warnings: Vec<_> = issues.iter().filter(|i| i.severity == "warning").collect();
        Ok(json!({
            "ok": errors.is_empty(),
            "error_count": errors.len(),
            "warning_count": warnings.len(),
            "errors": errors,
            "warnings": warnings,
        }))
    }

    pub fn validate_scenes(&self) -> Result<Value> {
        let mut issues = Vec::new();
        for entry in WalkDir::new(&self.project_path)
            .into_iter()
            .filter_map(|e| e.ok())
            .filter(|e| e.file_type().is_file())
        {
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) != Some("tscn") {
                continue;
            }
            let refs = tscn::parse_ext_resources(path).unwrap_or_default();
            for res in refs {
                if !self.resource_exists(&res) {
                    issues.push(ValidationIssue {
                        severity: "error".into(),
                        message: format!("missing ext_resource: {res}"),
                        file: Some(path.display().to_string()),
                        line: None,
                        resource: Some(res),
                    });
                }
            }
        }
        Ok(json!({
            "ok": issues.is_empty(),
            "issues": issues,
        }))
    }

    pub fn validate_gdextension(&self) -> Result<Value> {
        let mut issues = Vec::new();
        for entry in WalkDir::new(&self.project_path)
            .into_iter()
            .filter_map(|e| e.ok())
            .filter(|e| e.file_type().is_file())
        {
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) != Some("gdextension") {
                continue;
            }
            let content = std::fs::read_to_string(path)?;
            let re = Regex::new(r#"=\s*"(res://[^"]+)""#)?;
            for caps in re.captures_iter(&content) {
                if let Some(res) = caps.get(1) {
                    let res_path = res.as_str();
                    if !self.resource_exists(res_path) {
                        issues.push(ValidationIssue {
                            severity: "error".into(),
                            message: format!("missing gdextension library: {res_path}"),
                            file: Some(path.display().to_string()),
                            line: None,
                            resource: Some(res_path.to_string()),
                        });
                    }
                }
            }
        }
        Ok(json!({
            "ok": issues.is_empty(),
            "issues": issues,
        }))
    }

    pub fn validate_uids(&self) -> Result<Value> {
        let orphans = uid_index::find_orphan_uids(&self.project_path)?;
        let missing = uid_index::find_missing_uids(&self.project_path)?;
        let mut issues = Vec::new();
        for o in &orphans {
            issues.push(ValidationIssue {
                severity: "warning".into(),
                message: format!("orphan uid sidecar: {o}"),
                file: Some(o.clone()),
                line: None,
                resource: None,
            });
        }
        for m in &missing {
            issues.push(ValidationIssue {
                severity: "warning".into(),
                message: format!("missing uid sidecar for: {m}"),
                file: Some(m.clone()),
                line: None,
                resource: None,
            });
        }
        Ok(json!({
            "ok": issues.iter().all(|i| i.severity != "error"),
            "orphan_count": orphans.len(),
            "missing_count": missing.len(),
            "issues": issues,
        }))
    }

    pub fn validate_project(&self) -> Result<Value> {
        let resources = self.validate_resources()?;
        let scenes = self.validate_scenes()?;
        let gdext = self.validate_gdextension()?;
        let uids = self.validate_uids()?;

        let mut errors = Vec::new();
        let mut warnings = Vec::new();
        for section in [&resources, &scenes, &gdext, &uids] {
            if let Some(arr) = section.get("errors").and_then(|v| v.as_array()) {
                errors.extend(arr.iter().cloned());
            }
            if let Some(arr) = section.get("issues").and_then(|v| v.as_array()) {
                for item in arr {
                    if item.get("severity").and_then(|s| s.as_str()) == Some("error") {
                        errors.push(item.clone());
                    } else {
                        warnings.push(item.clone());
                    }
                }
            }
            if let Some(arr) = section.get("warnings").and_then(|v| v.as_array()) {
                warnings.extend(arr.iter().cloned());
            }
        }

        Ok(json!({
            "ok": errors.is_empty(),
            "error_count": errors.len(),
            "warning_count": warnings.len(),
            "errors": errors,
            "warnings": warnings,
            "sections": {
                "resources": resources,
                "scenes": scenes,
                "gdextension": gdext,
                "uids": uids,
            }
        }))
    }

    fn collect_resource_issues(&self) -> Result<Vec<ValidationIssue>> {
        let mut issues = Vec::new();
        let mut checked = HashSet::new();

        let pg = project_godot::parse(&self.project_path.join("project.godot"))?;
        if let Some(main) = &pg.main_scene {
            self.check_ref(main, "project.godot", None, &mut issues, &mut checked);
        }
        for plugin in &pg.editor_plugins {
            self.check_ref(plugin, "project.godot", None, &mut issues, &mut checked);
        }

        for entry in WalkDir::new(&self.project_path)
            .into_iter()
            .filter_map(|e| e.ok())
            .filter(|e| e.file_type().is_file())
        {
            let path = entry.path();
            let ext = path.extension().and_then(|e| e.to_str()).unwrap_or("");
            if !matches!(ext, "gd" | "tscn" | "gdextension" | "gdshader") {
                continue;
            }
            let content = match std::fs::read_to_string(path) {
                Ok(c) => c,
                Err(_) => continue,
            };
            let re = Regex::new(r#"res://[^"\')\s]+"#)?;
            for caps in re.captures_iter(&content) {
                if let Some(m) = caps.get(0) {
                    self.check_ref(
                        m.as_str(),
                        &path.display().to_string(),
                        None,
                        &mut issues,
                        &mut checked,
                    );
                }
            }
            if ext == "gd" {
                for (line_no, line) in content.lines().enumerate() {
                    for res in gdscript::extract_res_refs(line) {
                        self.check_ref(
                            &res,
                            &path.display().to_string(),
                            Some((line_no + 1) as u32),
                            &mut issues,
                            &mut checked,
                        );
                    }
                }
            }
        }
        Ok(issues)
    }

    fn check_ref(
        &self,
        res_path: &str,
        file: &str,
        line: Option<u32>,
        issues: &mut Vec<ValidationIssue>,
        checked: &mut HashSet<String>,
    ) {
        if res_path.contains('%') {
            return;
        }
        if !checked.insert(res_path.to_string()) {
            return;
        }
        if self.resource_exists(res_path) {
            return;
        }
        issues.push(ValidationIssue {
            severity: "error".into(),
            message: format!("missing resource: {res_path}"),
            file: Some(file.to_string()),
            line,
            resource: Some(res_path.to_string()),
        });
    }

    fn resource_exists(&self, res_path: &str) -> bool {
        if !res_path.starts_with("res://") {
            return false;
        }
        if res_path.contains('%') {
            return true;
        }
        res_to_abs(&self.project_path, res_path)
            .map(|p| p.exists())
            .unwrap_or(false)
    }
}
