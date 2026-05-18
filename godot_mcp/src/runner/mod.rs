use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

use anyhow::{anyhow, Context, Result};
use regex::Regex;
use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::process::Command;
use tokio::sync::Mutex;
use walkdir::WalkDir;

use crate::project::project_godot;

#[derive(Debug, Clone, serde::Serialize)]
pub struct RunResult {
    pub command: String,
    pub exit_code: Option<i32>,
    pub duration_ms: u64,
    pub stdout: String,
    pub stderr: String,
    pub metrics: Option<Value>,
}

#[derive(Clone)]
pub struct GodotRunner {
    project_path: PathBuf,
    workspace_root: PathBuf,
    godot_binary: Option<PathBuf>,
    stress_scene: Option<String>,
    extension_health_script: Option<String>,
    last_run: Arc<Mutex<Option<RunResult>>>,
    run_lock: Arc<Mutex<()>>,
}

impl GodotRunner {
    pub fn godot_binary(&self) -> Option<&PathBuf> {
        self.godot_binary.as_ref()
    }

    pub fn new(
        project_path: PathBuf,
        workspace_root: PathBuf,
        godot_binary: Option<PathBuf>,
        stress_scene: Option<String>,
        extension_health_script: Option<String>,
    ) -> Self {
        Self {
            project_path,
            workspace_root,
            godot_binary,
            stress_scene,
            extension_health_script,
            last_run: Arc::new(Mutex::new(None)),
            run_lock: Arc::new(Mutex::new(())),
        }
    }

    pub async fn last_output(&self) -> Value {
        let guard = self.last_run.lock().await;
        match guard.as_ref() {
            Some(r) => json!(r),
            None => json!({ "error": "no previous run" }),
        }
    }

    pub async fn run_script(&self, script_path: &str, timeout_secs: u64) -> Result<Value> {
        let _lock = self.run_lock.lock().await;
        let binary = self.resolve_binary()?;
        let args = vec![
            "--path".to_string(),
            self.project_path.display().to_string(),
            "--headless".to_string(),
            "--script".to_string(),
            script_path.to_string(),
        ];
        let result = self
            .execute(&binary, &args, Duration::from_secs(timeout_secs))
            .await?;
        self.store_run(&result).await;
        Ok(json!(result))
    }

    pub async fn run_scene(
        &self,
        scene_path: &str,
        quit_after_secs: u64,
        timeout_secs: u64,
    ) -> Result<Value> {
        let _lock = self.run_lock.lock().await;
        let binary = self.resolve_binary()?;
        let mut args = vec![
            "--path".to_string(),
            self.project_path.display().to_string(),
            "--headless".to_string(),
            scene_path.to_string(),
        ];
        if quit_after_secs > 0 {
            args.push("--quit-after".to_string());
            args.push(quit_after_secs.to_string());
        }
        let result = self
            .execute(&binary, &args, Duration::from_secs(timeout_secs))
            .await?;
        self.store_run(&result).await;
        Ok(json!(result))
    }

    pub async fn run_stress(
        &self,
        preset_name: &str,
        run_seconds: f64,
        timeout_secs: u64,
    ) -> Result<Value> {
        let _lock = self.run_lock.lock().await;
        let binary = self.resolve_binary()?;
        let stress_scene = self
            .stress_scene
            .as_deref()
            .ok_or_else(|| anyhow!("GODOT_STRESS_SCENE is not set"))?;
        let quit_after = (run_seconds.ceil() as u64 + 10).max(15);
        let args = vec![
            "--path".to_string(),
            self.project_path.display().to_string(),
            "--headless".to_string(),
            stress_scene.to_string(),
            "--quit-after".to_string(),
            quit_after.to_string(),
        ];

        let mut result = self
            .execute(&binary, &args, Duration::from_secs(timeout_secs))
            .await?;

        let metrics_before = find_latest_metrics(&self.project_path)?;
        tokio::time::sleep(Duration::from_millis(500)).await;
        let metrics = find_latest_metrics(&self.project_path)?;
        if metrics.is_some() && metrics != metrics_before {
            result.metrics = metrics;
        } else if let Some(m) = parse_metrics_from_stdout(&result.stdout) {
            result.metrics = Some(m);
        }

        let mut out = json!(result);
        if let Some(obj) = out.as_object_mut() {
            obj.insert("preset_name".into(), json!(preset_name));
            obj.insert("run_seconds".into(), json!(run_seconds));
        }
        self.store_run(&RunResult {
            metrics: out.get("metrics").cloned(),
            ..result
        })
        .await;
        Ok(out)
    }

    pub async fn run_test_suite(&self, timeout_secs: u64) -> Result<Value> {
        let tests = discover_tests(&self.project_path);
        let mut results = Vec::new();
        let mut passed = 0u32;
        let mut failed = 0u32;

        for test in &tests {
            let script = format!("res://{}", test);
            match self.run_script(&script, timeout_secs).await {
                Ok(v) => {
                    let exit = v.get("exit_code").and_then(|c| c.as_i64()).unwrap_or(-1);
                    let ok = exit == 0;
                    if ok {
                        passed += 1;
                    } else {
                        failed += 1;
                    }
                    results.push(json!({
                        "script": script,
                        "ok": ok,
                        "exit_code": exit,
                    }));
                }
                Err(err) => {
                    failed += 1;
                    results.push(json!({
                        "script": script,
                        "ok": false,
                        "error": err.to_string(),
                    }));
                }
            }
        }

        Ok(json!({
            "total": tests.len(),
            "passed": passed,
            "failed": failed,
            "results": results,
        }))
    }

    pub async fn run_build_extension(&self, timeout_secs: u64) -> Result<Value> {
        let _lock = self.run_lock.lock().await;
        let script = self.workspace_root.join("build_extension.sh");
        if !script.exists() {
            return Err(anyhow!("build_extension.sh not found"));
        }

        let output = tokio::time::timeout(
            Duration::from_secs(timeout_secs),
            Command::new("bash")
                .arg(&script)
                .current_dir(&self.workspace_root)
                .output(),
        )
        .await
        .context("build_extension timed out")??;

        let result = RunResult {
            command: script.display().to_string(),
            exit_code: output.status.code(),
            duration_ms: 0,
            stdout: String::from_utf8_lossy(&output.stdout).into_owned(),
            stderr: String::from_utf8_lossy(&output.stderr).into_owned(),
            metrics: None,
        };
        self.store_run(&result).await;
        Ok(json!(result))
    }

    pub async fn run_find_metrics(&self) -> Result<Value> {
        find_latest_metrics(&self.project_path)
            .map(|m| m.unwrap_or_else(|| json!({ "error": "no metrics files found" })))
    }

    pub async fn query_rust_extension(&self, timeout_secs: u64) -> Result<Value> {
        let script = self
            .extension_health_script
            .as_deref()
            .ok_or_else(|| anyhow!("GODOT_EXTENSION_HEALTH_SCRIPT is not set"))?;
        let check_path = crate::util::resolve_res_path(&self.project_path, script)?;
        if !check_path.exists() {
            return Ok(json!({
                "ready": false,
                "error": format!("{script} not installed"),
            }));
        }
        self.run_script(script, timeout_secs).await
    }

    pub async fn compare_metrics(&self, file_a: &str, file_b: &str) -> Result<Value> {
        let a = read_metrics_file(file_a)?;
        let b = read_metrics_file(file_b)?;
        let mut diff = serde_json::Map::new();
        if let (Some(ao), Some(bo)) = (a.as_object(), b.as_object()) {
            for (key, va) in ao {
                if key == "path" {
                    continue;
                }
                let vb = bo.get(key).cloned().unwrap_or(Value::Null);
                diff.insert(
                    key.clone(),
                    json!({ "a": va, "b": vb, "delta": metric_delta(va, &vb) }),
                );
            }
        }
        Ok(json!({ "a": a, "b": b, "diff": diff }))
    }

    fn resolve_binary(&self) -> Result<PathBuf> {
        self.godot_binary
            .clone()
            .filter(|p| p.exists())
            .ok_or_else(|| anyhow!("Godot binary not found; set GODOT_BINARY"))
    }

    async fn execute(
        &self,
        binary: &Path,
        args: &[String],
        timeout: Duration,
    ) -> Result<RunResult> {
        let started = Instant::now();
        let mut cmd = Command::new(binary);
        cmd.args(args)
            .current_dir(&self.project_path)
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .kill_on_drop(true);

        let mut child = cmd.spawn().context("spawn Godot")?;
        let stdout = child.stdout.take().context("stdout pipe")?;
        let stderr = child.stderr.take().context("stderr pipe")?;

        let stdout_task = tokio::spawn(async move {
            let mut lines = BufReader::new(stdout).lines();
            let mut out = String::new();
            while let Ok(Some(line)) = lines.next_line().await {
                out.push_str(&line);
                out.push('\n');
            }
            out
        });
        let stderr_task = tokio::spawn(async move {
            let mut lines = BufReader::new(stderr).lines();
            let mut out = String::new();
            while let Ok(Some(line)) = lines.next_line().await {
                out.push_str(&line);
                out.push('\n');
            }
            out
        });

        let status = tokio::time::timeout(timeout, child.wait())
            .await
            .context("Godot run timed out")??;

        let stdout = stdout_task.await.unwrap_or_default();
        let stderr = stderr_task.await.unwrap_or_default();

        Ok(RunResult {
            command: format!("{} {}", binary.display(), args.join(" ")),
            exit_code: status.code(),
            duration_ms: started.elapsed().as_millis() as u64,
            stdout,
            stderr,
            metrics: None,
        })
    }

    async fn store_run(&self, result: &RunResult) {
        *self.last_run.lock().await = Some(result.clone());
    }
}

fn discover_tests(project_path: &Path) -> Vec<String> {
    let tests_dir = project_path.join("scripts/tests");
    let mut tests = Vec::new();
    if !tests_dir.exists() {
        return tests;
    }
    for entry in WalkDir::new(&tests_dir)
        .max_depth(1)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().is_file())
    {
        let name = entry.file_name().to_string_lossy();
        if name.starts_with('_') {
            continue;
        }
        if name.ends_with("Runner.gd")
            || name.ends_with("Check.gd")
            || name.ends_with("Test.gd")
            || name.ends_with("Breakdown.gd")
        {
            if let Ok(rel) = entry.path().strip_prefix(project_path) {
                tests.push(rel.display().to_string().replace('\\', "/"));
            }
        }
    }
    tests.sort();
    tests
}

fn stress_metrics_dir(project_path: &Path) -> Result<PathBuf> {
    let pg = project_godot::parse(&project_path.join("project.godot"))?;
    let app_name = pg.name;

    #[cfg(target_os = "macos")]
    {
        let home = dirs_home().ok_or_else(|| anyhow!("HOME not set"))?;
        Ok(home
            .join("Library/Application Support/Godot/app_userdata")
            .join(app_name)
            .join("stress_metrics"))
    }

    #[cfg(target_os = "linux")]
    {
        let home = dirs_home().ok_or_else(|| anyhow!("HOME not set"))?;
        Ok(home
            .join(".local/share/godot/app_userdata")
            .join(app_name)
            .join("stress_metrics"))
    }

    #[cfg(target_os = "windows")]
    {
        let appdata = std::env::var("APPDATA").context("APPDATA not set")?;
        Ok(PathBuf::from(appdata)
            .join("Godot/app_userdata")
            .join(app_name)
            .join("stress_metrics"))
    }

    #[cfg(not(any(target_os = "macos", target_os = "linux", target_os = "windows")))]
    {
        let _ = app_name;
        Err(anyhow!("unsupported OS for metrics path"))
    }
}

fn dirs_home() -> Option<PathBuf> {
    std::env::var("HOME")
        .ok()
        .map(PathBuf::from)
        .or_else(|| std::env::var("USERPROFILE").ok().map(PathBuf::from))
}

fn find_latest_metrics(project_path: &Path) -> Result<Option<Value>> {
    let dir = stress_metrics_dir(project_path)?;
    if !dir.exists() {
        return Ok(None);
    }
    let mut latest: Option<(std::time::SystemTime, PathBuf)> = None;
    for entry in std::fs::read_dir(&dir)? {
        let entry = entry?;
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("json") {
            continue;
        }
        let modified = entry.metadata()?.modified()?;
        if latest.as_ref().map(|(t, _)| modified > *t).unwrap_or(true) {
            latest = Some((modified, path));
        }
    }
    if let Some((_, path)) = latest {
        let content = std::fs::read_to_string(&path)?;
        let metrics: Value = serde_json::from_str(&content)?;
        return Ok(Some(json!({ "path": path, "metrics": metrics })));
    }
    Ok(None)
}

fn read_metrics_file(path: &str) -> Result<Value> {
    let content = std::fs::read_to_string(path)?;
    Ok(serde_json::from_str(&content)?)
}

fn parse_metrics_from_stdout(stdout: &str) -> Option<Value> {
    for line in stdout.lines() {
        if line.contains("Stress metrics written:") {
            let re = Regex::new(r"(\{.*\})").ok()?;
            if let Some(caps) = re.captures(line) {
                if let Ok(v) = serde_json::from_str(caps.get(1)?.as_str()) {
                    return Some(v);
                }
            }
        }
    }
    None
}

fn metric_delta(a: &Value, b: &Value) -> Value {
    match (a.as_f64(), b.as_f64()) {
        (Some(x), Some(y)) => json!(y - x),
        _ => Value::Null,
    }
}
