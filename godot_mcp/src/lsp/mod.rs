mod client;

pub use client::GodotLspClient;

use std::path::PathBuf;

#[derive(Clone, Debug)]
pub struct LspConfig {
    pub host: String,
    pub port: u16,
    pub project_path: PathBuf,
    pub diag_wait_ms: u64,
    pub request_timeout_secs: u64,
}

impl LspConfig {
    pub fn root_uri(&self) -> String {
        crate::util::path_to_uri(&self.project_path)
    }
}
