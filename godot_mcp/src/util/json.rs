use rmcp::{model::*, ErrorData as McpError};
use serde_json::Value;

pub fn json_result(value: Value) -> Result<CallToolResult, McpError> {
    Ok(CallToolResult::success(vec![ContentBlock::text(
        serde_json::to_string_pretty(&value).unwrap_or_else(|_| value.to_string()),
    )]))
}

pub fn tool_error(err: impl std::fmt::Display) -> McpError {
    McpError::internal_error(format!("{err}"), None)
}
