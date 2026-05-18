pub mod json;
pub mod paths;

pub use json::{json_result, tool_error};
pub use paths::{abs_to_res, path_to_uri, res_to_abs, resolve_res_path};
