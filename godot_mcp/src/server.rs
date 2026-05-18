use std::sync::Arc;

use rmcp::{
    handler::server::wrapper::Parameters, model::*, schemars, tool, tool_handler, tool_router,
    ErrorData as McpError, ServerHandler,
};
use serde::Deserialize;

use crate::editor::EditorRpcClient;
use crate::lsp::GodotLspClient;
use crate::project::ProjectIndex;
use crate::runner::GodotRunner;
use crate::util::{json_result, tool_error};
use crate::validate::Validator;

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct FilePathArgs {
    /// Absolute path, project-relative path, or res:// URI.
    pub file_path: String,
    #[serde(default)]
    pub refresh: bool,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct PositionArgs {
    pub file_path: String,
    pub line: u32,
    pub character: u32,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct WorkspaceSymbolArgs {
    #[serde(default)]
    pub query: String,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct ScenePathArgs {
    pub scene_path: String,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct RunScriptArgs {
    pub script_path: String,
    #[serde(default = "default_timeout")]
    pub timeout_secs: u64,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct RunSceneArgs {
    pub scene_path: String,
    #[serde(default = "default_quit_after")]
    pub quit_after_secs: u64,
    #[serde(default = "default_timeout")]
    pub timeout_secs: u64,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct RunStressArgs {
    #[serde(default = "default_preset")]
    pub preset_name: String,
    #[serde(default = "default_run_seconds")]
    pub run_seconds: f64,
    #[serde(default = "default_stress_timeout")]
    pub timeout_secs: u64,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct RunTestSuiteArgs {
    #[serde(default = "default_timeout")]
    pub timeout_secs: u64,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct RunBuildArgs {
    #[serde(default = "default_build_timeout")]
    pub timeout_secs: u64,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct CompareMetricsArgs {
    pub file_a: String,
    pub file_b: String,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct OptionalScenePathArgs {
    pub scene_path: Option<String>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct OptionalNodePathArgs {
    pub node_path: Option<String>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct ResourcePathArgs {
    pub resource_path: String,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct CaptureScreenshotArgs {
    pub output_path: Option<String>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct SimulateInputArgs {
    pub action: String,
    #[serde(default = "default_pressed")]
    pub pressed: bool,
}

fn default_timeout() -> u64 {
    120
}
fn default_quit_after() -> u64 {
    65
}
fn default_preset() -> String {
    "default".into()
}
fn default_run_seconds() -> f64 {
    60.0
}
fn default_stress_timeout() -> u64 {
    180
}
fn default_build_timeout() -> u64 {
    300
}
fn default_pressed() -> bool {
    true
}

#[derive(Clone)]
pub struct GodotMcp {
    lsp: Arc<GodotLspClient>,
    editor: Arc<EditorRpcClient>,
    project: ProjectIndex,
    validator: Validator,
    runner: GodotRunner,
    #[allow(dead_code)]
    tool_router: rmcp::handler::server::router::tool::ToolRouter<Self>,
}

#[tool_router]
impl GodotMcp {
    pub fn new(
        lsp: Arc<GodotLspClient>,
        editor: Arc<EditorRpcClient>,
        project: ProjectIndex,
        validator: Validator,
        runner: GodotRunner,
    ) -> Self {
        Self {
            lsp,
            editor,
            project,
            validator,
            runner,
            tool_router: Self::tool_router(),
        }
    }

    #[tool(description = "Unified health check: LSP, editor RPC, Godot binary, project path.")]
    async fn godot_status(&self) -> Result<CallToolResult, McpError> {
        let lsp = self.lsp.status().await.map_err(tool_error)?;
        let editor = self.editor.status().await.map_err(tool_error)?;
        let binary = self.runner.godot_binary().map(|p| p.display().to_string());
        json_result(serde_json::json!({
            "project_path": self.project.project_path,
            "godot_binary": binary,
            "lsp": lsp,
            "editor": editor,
        }))
    }

    // --- LSP tools ---

    #[tool(description = "Check Godot GDScript language server connectivity.")]
    async fn lsp_status(&self) -> Result<CallToolResult, McpError> {
        json_result(self.lsp.status().await.map_err(tool_error)?)
    }

    #[tool(description = "Get GDScript diagnostics for a file.")]
    async fn lsp_diagnostics(
        &self,
        Parameters(args): Parameters<FilePathArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.lsp
                .diagnostics(&args.file_path, args.refresh)
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Get hover information at a position.")]
    async fn lsp_hover(
        &self,
        Parameters(args): Parameters<PositionArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.lsp
                .hover(&args.file_path, args.line, args.character)
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Get go-to-definition at a position.")]
    async fn lsp_definition(
        &self,
        Parameters(args): Parameters<PositionArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.lsp
                .definition(&args.file_path, args.line, args.character)
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Get code completions at a position.")]
    async fn lsp_completion(
        &self,
        Parameters(args): Parameters<PositionArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.lsp
                .completion(&args.file_path, args.line, args.character)
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "List document symbols for a GDScript file.")]
    async fn lsp_document_symbols(
        &self,
        Parameters(args): Parameters<FilePathArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.lsp
                .document_symbols(&args.file_path)
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Find references at a position.")]
    async fn lsp_references(
        &self,
        Parameters(args): Parameters<PositionArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.lsp
                .references(&args.file_path, args.line, args.character)
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Search workspace symbols.")]
    async fn lsp_workspace_symbols(
        &self,
        Parameters(args): Parameters<WorkspaceSymbolArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.lsp
                .workspace_symbols(&args.query)
                .await
                .map_err(tool_error)?,
        )
    }

    // --- Project tools ---

    #[tool(description = "Parse project.godot settings.")]
    async fn project_info(&self) -> Result<CallToolResult, McpError> {
        json_result(self.project.info().map_err(tool_error)?)
    }

    #[tool(description = "List all .tscn scenes in the project.")]
    async fn project_list_scenes(&self) -> Result<CallToolResult, McpError> {
        json_result(self.project.list_scenes().map_err(tool_error)?)
    }

    #[tool(description = "List all .gd scripts in the project.")]
    async fn project_list_scripts(&self) -> Result<CallToolResult, McpError> {
        json_result(self.project.list_scripts().map_err(tool_error)?)
    }

    #[tool(description = "Parse node hierarchy for a scene file.")]
    async fn project_scene_tree(
        &self,
        Parameters(args): Parameters<FilePathArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.project
                .scene_tree(&args.file_path)
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Build scene/script dependency graph.")]
    async fn project_dependencies(&self) -> Result<CallToolResult, McpError> {
        json_result(self.project.dependencies().map_err(tool_error)?)
    }

    #[tool(description = "List registered GDScript class_name entries.")]
    async fn project_class_registry(&self) -> Result<CallToolResult, McpError> {
        json_result(self.project.class_registry().map_err(tool_error)?)
    }

    #[tool(description = "List input map action names.")]
    async fn project_input_map(&self) -> Result<CallToolResult, McpError> {
        json_result(self.project.input_map().map_err(tool_error)?)
    }

    #[tool(description = "Build UID sidecar index.")]
    async fn project_uid_index(&self) -> Result<CallToolResult, McpError> {
        json_result(self.project.uid_index().map_err(tool_error)?)
    }

    // --- Validation tools ---

    #[tool(description = "Check all res:// references resolve to existing files.")]
    async fn validate_resources(&self) -> Result<CallToolResult, McpError> {
        json_result(self.validator.validate_resources().map_err(tool_error)?)
    }

    #[tool(description = "Validate ext_resource paths in all scenes.")]
    async fn validate_scenes(&self) -> Result<CallToolResult, McpError> {
        json_result(self.validator.validate_scenes().map_err(tool_error)?)
    }

    #[tool(description = "Validate GDExtension native libraries exist.")]
    async fn validate_gdextension(&self) -> Result<CallToolResult, McpError> {
        json_result(self.validator.validate_gdextension().map_err(tool_error)?)
    }

    #[tool(description = "Check UID sidecar consistency.")]
    async fn validate_uids(&self) -> Result<CallToolResult, McpError> {
        json_result(self.validator.validate_uids().map_err(tool_error)?)
    }

    #[tool(description = "Run all validation checks.")]
    async fn validate_project(&self) -> Result<CallToolResult, McpError> {
        json_result(self.validator.validate_project().map_err(tool_error)?)
    }

    // --- Runner tools ---

    #[tool(description = "Run a headless SceneTree script.")]
    async fn run_script(
        &self,
        Parameters(args): Parameters<RunScriptArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.runner
                .run_script(&args.script_path, args.timeout_secs)
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Run a scene headless with optional --quit-after.")]
    async fn run_scene(
        &self,
        Parameters(args): Parameters<RunSceneArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.runner
                .run_scene(&args.scene_path, args.quit_after_secs, args.timeout_secs)
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Run configured benchmark scene and collect metrics.")]
    async fn run_stress(
        &self,
        Parameters(args): Parameters<RunStressArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.runner
                .run_stress(&args.preset_name, args.run_seconds, args.timeout_secs)
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Run all discovered test scripts.")]
    async fn run_test_suite(
        &self,
        Parameters(args): Parameters<RunTestSuiteArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.runner
                .run_test_suite(args.timeout_secs)
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Run build_extension.sh.")]
    async fn run_build_extension(
        &self,
        Parameters(args): Parameters<RunBuildArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.runner
                .run_build_extension(args.timeout_secs)
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Get stdout/stderr from the most recent run.")]
    async fn run_last_output(&self) -> Result<CallToolResult, McpError> {
        json_result(self.runner.last_output().await)
    }

    #[tool(description = "Find latest benchmark metrics JSON.")]
    async fn run_find_metrics(&self) -> Result<CallToolResult, McpError> {
        json_result(self.runner.run_find_metrics().await.map_err(tool_error)?)
    }

    // --- Editor tools ---

    #[tool(description = "Check Godot editor RPC bridge connectivity.")]
    async fn editor_status(&self) -> Result<CallToolResult, McpError> {
        json_result(self.editor.status().await.map_err(tool_error)?)
    }

    #[tool(description = "List open scene tabs in the editor.")]
    async fn editor_get_open_scenes(&self) -> Result<CallToolResult, McpError> {
        json_result(self.editor.get_open_scenes().await.map_err(tool_error)?)
    }

    #[tool(description = "Get the currently edited scene.")]
    async fn editor_get_current_scene(&self) -> Result<CallToolResult, McpError> {
        json_result(self.editor.get_current_scene().await.map_err(tool_error)?)
    }

    #[tool(description = "Get selected nodes in the editor.")]
    async fn editor_get_selection(&self) -> Result<CallToolResult, McpError> {
        json_result(self.editor.get_selection().await.map_err(tool_error)?)
    }

    #[tool(description = "Open a scene in the editor.")]
    async fn editor_open_scene(
        &self,
        Parameters(args): Parameters<ScenePathArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.editor
                .open_scene(&args.scene_path)
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Save the current scene in the editor.")]
    async fn editor_save_scene(&self) -> Result<CallToolResult, McpError> {
        json_result(self.editor.save_scene().await.map_err(tool_error)?)
    }

    #[tool(description = "Play current or specified scene.")]
    async fn editor_play(
        &self,
        Parameters(args): Parameters<OptionalScenePathArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.editor
                .play_scene(args.scene_path.as_deref())
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Stop play mode in the editor.")]
    async fn editor_stop(&self) -> Result<CallToolResult, McpError> {
        json_result(self.editor.stop_play().await.map_err(tool_error)?)
    }

    #[tool(description = "Get live scene tree from the editor.")]
    async fn editor_get_scene_tree(&self) -> Result<CallToolResult, McpError> {
        json_result(self.editor.get_scene_tree().await.map_err(tool_error)?)
    }

    #[tool(description = "Get properties for selected or specified node.")]
    async fn editor_get_node_properties(
        &self,
        Parameters(args): Parameters<OptionalNodePathArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.editor
                .get_node_properties(args.node_path.as_deref())
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Get recent editor output log lines.")]
    async fn editor_get_output_log(&self) -> Result<CallToolResult, McpError> {
        json_result(self.editor.get_output_log().await.map_err(tool_error)?)
    }

    #[tool(description = "Load and inspect a resource.")]
    async fn editor_inspect_resource(
        &self,
        Parameters(args): Parameters<ResourcePathArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.editor
                .inspect_resource(&args.resource_path)
                .await
                .map_err(tool_error)?,
        )
    }

    // --- Advanced tools ---

    #[tool(description = "Capture editor viewport screenshot.")]
    async fn capture_screenshot(
        &self,
        Parameters(args): Parameters<CaptureScreenshotArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.editor
                .capture_screenshot(args.output_path.as_deref())
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Get runtime scene tree while playing.")]
    async fn get_runtime_scene_tree(&self) -> Result<CallToolResult, McpError> {
        json_result(
            self.editor
                .get_runtime_scene_tree()
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Simulate an input action during play mode.")]
    async fn simulate_input(
        &self,
        Parameters(args): Parameters<SimulateInputArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.editor
                .simulate_input(&args.action, args.pressed)
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Run configured GDExtension health check script headless.")]
    async fn query_rust_extension(&self) -> Result<CallToolResult, McpError> {
        json_result(
            self.runner
                .query_rust_extension(60)
                .await
                .map_err(tool_error)?,
        )
    }

    #[tool(description = "Compare two stress metrics JSON files.")]
    async fn compare_metrics(
        &self,
        Parameters(args): Parameters<CompareMetricsArgs>,
    ) -> Result<CallToolResult, McpError> {
        json_result(
            self.runner
                .compare_metrics(&args.file_a, &args.file_b)
                .await
                .map_err(tool_error)?,
        )
    }
}

#[tool_handler]
impl ServerHandler for GodotMcp {
    fn get_info(&self) -> ServerInfo {
        ServerInfo::new(ServerCapabilities::builder().enable_tools().build())
            .with_server_info(Implementation::from_build_env())
            .with_instructions(
                "Unified Godot MCP. \
                 Call godot_status first. \
                 Static tools (project_*, validate_*) need only filesystem. \
                 lsp_* needs Godot editor + LSP (port 6005). \
                 editor_* needs Godot editor + godot_mcp_bridge plugin (port 6007). \
                 run_* spawns Godot CLI headless. Lines/chars are zero-based for LSP. \
                 Optional benchmark and extension tools require GODOT_STRESS_SCENE or GODOT_EXTENSION_HEALTH_SCRIPT."
                    .to_string(),
            )
    }
}
