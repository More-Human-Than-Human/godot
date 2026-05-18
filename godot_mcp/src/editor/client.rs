use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::Arc;
use std::time::Duration;

use anyhow::{anyhow, Context, Result};
use serde_json::{json, Value};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::sync::Mutex;

use super::EditorConfig;

pub struct EditorRpcClient {
    config: EditorConfig,
    stream: Mutex<Option<TcpStream>>,
}

impl EditorRpcClient {
    pub fn new(config: EditorConfig) -> Arc<Self> {
        Arc::new(Self {
            config,
            stream: Mutex::new(None),
        })
    }

    pub async fn status(&self) -> Result<Value> {
        match self.call("editor.status", json!({})).await {
            Ok(result) => Ok(json!({
                "connected": true,
                "host": self.config.host,
                "port": self.config.port,
                "project_path": self.config.project_path,
                "editor": result,
            })),
            Err(err) => Ok(json!({
                "connected": false,
                "host": self.config.host,
                "port": self.config.port,
                "project_path": self.config.project_path,
                "error": err.to_string(),
            })),
        }
    }

    pub async fn get_open_scenes(&self) -> Result<Value> {
        self.call("editor.get_open_scenes", json!({})).await
    }

    pub async fn get_current_scene(&self) -> Result<Value> {
        self.call("editor.get_current_scene", json!({})).await
    }

    pub async fn get_selection(&self) -> Result<Value> {
        self.call("editor.get_selection", json!({})).await
    }

    pub async fn open_scene(&self, scene_path: &str) -> Result<Value> {
        self.call("editor.open_scene", json!({ "path": scene_path }))
            .await
    }

    pub async fn save_scene(&self) -> Result<Value> {
        self.call("editor.save_scene", json!({})).await
    }

    pub async fn play_scene(&self, scene_path: Option<&str>) -> Result<Value> {
        self.call("editor.play_scene", json!({ "path": scene_path }))
            .await
    }

    pub async fn stop_play(&self) -> Result<Value> {
        self.call("editor.stop_play", json!({})).await
    }

    pub async fn get_scene_tree(&self) -> Result<Value> {
        self.call("editor.get_scene_tree", json!({})).await
    }

    pub async fn get_node_properties(&self, node_path: Option<&str>) -> Result<Value> {
        self.call(
            "editor.get_node_properties",
            json!({ "node_path": node_path }),
        )
        .await
    }

    pub async fn inspect_resource(&self, resource_path: &str) -> Result<Value> {
        self.call("editor.inspect_resource", json!({ "path": resource_path }))
            .await
    }

    pub async fn get_output_log(&self) -> Result<Value> {
        self.call("editor.get_output_log", json!({})).await
    }

    pub async fn capture_screenshot(&self, output_path: Option<&str>) -> Result<Value> {
        self.call(
            "editor.capture_screenshot",
            json!({ "output_path": output_path }),
        )
        .await
    }

    pub async fn get_runtime_scene_tree(&self) -> Result<Value> {
        self.call("editor.get_runtime_scene_tree", json!({})).await
    }

    pub async fn simulate_input(&self, action: &str, pressed: bool) -> Result<Value> {
        self.call(
            "editor.simulate_input",
            json!({ "action": action, "pressed": pressed }),
        )
        .await
    }

    async fn call(&self, method: &str, params: Value) -> Result<Value> {
        self.ensure_connected().await?;
        static NEXT_ID: AtomicI64 = AtomicI64::new(1);
        let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);

        let message = json!({
            "jsonrpc": "2.0",
            "id": id,
            "method": method,
            "params": params,
        });

        self.send_message(&message).await?;
        self.read_response(id).await
    }

    async fn ensure_connected(&self) -> Result<()> {
        let mut guard = self.stream.lock().await;
        if guard.is_some() {
            return Ok(());
        }
        let addr = format!("{}:{}", self.config.host, self.config.port);
        let stream = TcpStream::connect(&addr)
            .await
            .with_context(|| format!("connect to Godot editor RPC at {addr}"))?;
        *guard = Some(stream);
        Ok(())
    }

    async fn send_message(&self, message: &Value) -> Result<()> {
        let body = serde_json::to_string(message)?;
        let frame = format!("Content-Length: {}\r\n\r\n{}", body.len(), body);
        let mut guard = self.stream.lock().await;
        let stream = guard
            .as_mut()
            .ok_or_else(|| anyhow!("not connected to editor RPC"))?;
        stream.write_all(frame.as_bytes()).await?;
        stream.flush().await?;
        Ok(())
    }

    async fn read_response(&self, id: i64) -> Result<Value> {
        let timeout = Duration::from_secs(self.config.request_timeout_secs);
        let deadline = tokio::time::Instant::now() + timeout;
        let mut buf = Vec::new();
        let mut scratch = [0u8; 8192];

        loop {
            if tokio::time::Instant::now() >= deadline {
                return Err(anyhow!("editor RPC timed out"));
            }

            let mut guard = self.stream.lock().await;
            let stream = guard
                .as_mut()
                .ok_or_else(|| anyhow!("not connected to editor RPC"))?;

            let read_fut = stream.read(&mut scratch);
            let n = tokio::time::timeout(deadline - tokio::time::Instant::now(), read_fut)
                .await
                .context("editor RPC read timeout")??;

            if n == 0 {
                *guard = None;
                return Err(anyhow!("editor RPC connection closed"));
            }
            buf.extend_from_slice(&scratch[..n]);
            drop(guard);

            while let Some(message) = parse_message(&mut buf)? {
                if message.get("id").and_then(Value::as_i64) == Some(id) {
                    if let Some(err) = message.get("error") {
                        return Err(anyhow!("editor RPC error: {err}"));
                    }
                    return Ok(message.get("result").cloned().unwrap_or(Value::Null));
                }
            }
        }
    }
}

fn parse_message(buf: &mut Vec<u8>) -> Result<Option<Value>> {
    let Some(header_end) = buf.windows(4).position(|w| w == b"\r\n\r\n") else {
        return Ok(None);
    };

    let header = std::str::from_utf8(&buf[..header_end]).context("invalid RPC header")?;
    let content_length = header
        .lines()
        .find_map(|line| line.strip_prefix("Content-Length:"))
        .context("missing Content-Length")?
        .trim()
        .parse::<usize>()
        .context("invalid Content-Length")?;

    let body_start = header_end + 4;
    if buf.len() < body_start + content_length {
        return Ok(None);
    }

    let body = &buf[body_start..body_start + content_length];
    let message: Value = serde_json::from_slice(body).context("invalid RPC JSON body")?;
    buf.drain(..body_start + content_length);
    Ok(Some(message))
}
