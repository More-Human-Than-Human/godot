use std::path::{Path, PathBuf};

#[derive(Clone, Debug)]
pub struct GodotConfig {
    pub project_path: PathBuf,
    pub godot_binary: Option<PathBuf>,
    pub stress_scene: Option<String>,
    pub extension_health_script: Option<String>,
    pub lsp_host: String,
    pub lsp_port: u16,
    pub editor_rpc_host: String,
    pub editor_rpc_port: u16,
    pub lsp_diag_wait_ms: u64,
    pub lsp_request_timeout_secs: u64,
    pub workspace_root: PathBuf,
}

impl GodotConfig {
    pub fn from_env() -> Self {
        let workspace_root = std::env::current_dir().unwrap_or_default();
        let project_path = std::env::var("GODOT_PROJECT_PATH")
            .map(PathBuf::from)
            .unwrap_or_else(|_| workspace_root.join("godot"));

        let godot_binary = std::env::var("GODOT_BINARY")
            .ok()
            .map(PathBuf::from)
            .filter(|p| p.exists())
            .or_else(resolve_godot_binary);

        Self {
            project_path,
            godot_binary,
            stress_scene: std::env::var("GODOT_STRESS_SCENE").ok(),
            extension_health_script: std::env::var("GODOT_EXTENSION_HEALTH_SCRIPT").ok(),
            lsp_host: std::env::var("GODOT_LSP_HOST").unwrap_or_else(|_| "127.0.0.1".into()),
            lsp_port: env_u16("GODOT_LSP_PORT", 6005),
            editor_rpc_host: std::env::var("GODOT_EDITOR_RPC_HOST")
                .unwrap_or_else(|_| "127.0.0.1".into()),
            editor_rpc_port: env_u16("GODOT_EDITOR_RPC_PORT", 6007),
            lsp_diag_wait_ms: env_u64("GODOT_LSP_DIAG_WAIT_MS", 250),
            lsp_request_timeout_secs: env_u64("GODOT_LSP_REQUEST_TIMEOUT_SECS", 10),
            workspace_root,
        }
    }

    pub fn lsp_config(&self) -> crate::lsp::LspConfig {
        crate::lsp::LspConfig {
            host: self.lsp_host.clone(),
            port: self.lsp_port,
            project_path: self.project_path.clone(),
            diag_wait_ms: self.lsp_diag_wait_ms,
            request_timeout_secs: self.lsp_request_timeout_secs,
        }
    }

    pub fn editor_config(&self) -> crate::editor::EditorConfig {
        crate::editor::EditorConfig {
            host: self.editor_rpc_host.clone(),
            port: self.editor_rpc_port,
            project_path: self.project_path.clone(),
            request_timeout_secs: self.lsp_request_timeout_secs,
        }
    }
}

fn env_u16(key: &str, default: u16) -> u16 {
    std::env::var(key)
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

fn env_u64(key: &str, default: u64) -> u64 {
    std::env::var(key)
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

fn resolve_godot_binary() -> Option<PathBuf> {
    let candidates = [
        "/Applications/Godot.app/Contents/MacOS/Godot",
        "/Applications/Godot 4.app/Contents/MacOS/Godot",
    ];
    for c in candidates {
        let p = PathBuf::from(c);
        if p.exists() {
            return Some(p);
        }
    }
    for cmd in ["godot4", "godot"] {
        if let Ok(path) = which(cmd) {
            return Some(path);
        }
    }
    None
}

fn which(cmd: &str) -> Result<PathBuf, ()> {
    let path_var = std::env::var("PATH").map_err(|_| ())?;
    for dir in path_var.split(':') {
        let candidate = Path::new(dir).join(cmd);
        if candidate.is_file() {
            return Ok(candidate);
        }
    }
    Err(())
}
