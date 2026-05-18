mod config;
mod editor;
mod lsp;
mod project;
mod runner;
mod server;
mod util;
mod validate;

use anyhow::Result;
use editor::EditorRpcClient;
use lsp::GodotLspClient;
use project::ProjectIndex;
use rmcp::{transport::stdio, ServiceExt};
use runner::GodotRunner;
use server::GodotMcp;
use tracing_subscriber::{fmt, EnvFilter};
use validate::Validator;

#[tokio::main]
async fn main() -> Result<()> {
    fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info")),
        )
        .with_writer(std::io::stderr)
        .with_ansi(false)
        .init();

    let config = config::GodotConfig::from_env();
    tracing::info!(
        project = %config.project_path.display(),
        lsp_port = config.lsp_port,
        editor_rpc_port = config.editor_rpc_port,
        "starting Godot MCP server"
    );

    let lsp = GodotLspClient::new(config.lsp_config());
    let editor = EditorRpcClient::new(config.editor_config());
    let project = ProjectIndex::new(config.project_path.clone());
    let validator = Validator::new(config.project_path.clone());
    let runner = GodotRunner::new(
        config.project_path.clone(),
        config.workspace_root.clone(),
        config.godot_binary.clone(),
        config.stress_scene.clone(),
        config.extension_health_script.clone(),
    );

    let service = GodotMcp::new(lsp, editor, project, validator, runner)
        .serve(stdio())
        .await
        .inspect_err(|err| tracing::error!("MCP serve error: {err:?}"))?;

    service.waiting().await?;
    Ok(())
}
