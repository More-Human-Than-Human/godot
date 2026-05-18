mod client;

pub use client::EditorRpcClient;

use std::path::PathBuf;

#[derive(Clone, Debug)]
pub struct EditorConfig {
    pub host: String,
    pub port: u16,
    pub project_path: PathBuf,
    pub request_timeout_secs: u64,
}
