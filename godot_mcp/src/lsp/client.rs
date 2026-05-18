use std::collections::HashMap;
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::Arc;
use std::time::Duration;

use anyhow::{anyhow, Context, Result};
use serde_json::{json, Value};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::sync::{mpsc, oneshot, Mutex};
use tokio::time::sleep;

use super::LspConfig;
use crate::util::resolve_res_path;

pub struct GodotLspClient {
    config: LspConfig,
    write_tx: Mutex<Option<mpsc::Sender<String>>>,
    pending: Arc<Mutex<HashMap<i64, oneshot::Sender<Value>>>>,
    diagnostics: Arc<Mutex<HashMap<String, Value>>>,
    open_versions: Arc<Mutex<HashMap<String, i32>>>,
    initialized: Arc<Mutex<bool>>,
}

impl GodotLspClient {
    pub fn new(config: LspConfig) -> Arc<Self> {
        Arc::new(Self {
            config,
            write_tx: Mutex::new(None),
            pending: Arc::new(Mutex::new(HashMap::new())),
            diagnostics: Arc::new(Mutex::new(HashMap::new())),
            open_versions: Arc::new(Mutex::new(HashMap::new())),
            initialized: Arc::new(Mutex::new(false)),
        })
    }

    pub async fn status(&self) -> Result<Value> {
        match self.ensure_connected().await {
            Ok(()) => Ok(json!({
                "connected": true,
                "host": self.config.host,
                "port": self.config.port,
                "project_path": self.config.project_path,
                "root_uri": self.config.root_uri(),
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

    pub async fn ensure_document_open(&self, file_path: &str) -> Result<String> {
        let abs = resolve_res_path(&self.config.project_path, file_path)?;
        let uri = crate::util::path_to_uri(&abs);
        let text =
            std::fs::read_to_string(&abs).with_context(|| format!("failed to read {abs:?}"))?;

        self.ensure_connected().await?;

        let version = {
            let mut versions = self.open_versions.lock().await;
            let version = versions.get(&uri).copied().unwrap_or(0) + 1;
            versions.insert(uri.clone(), version);
            version
        };

        if version == 1 {
            self.notify(
                "textDocument/didOpen",
                json!({
                    "textDocument": {
                        "uri": uri,
                        "languageId": "gdscript",
                        "version": version,
                        "text": text,
                    }
                }),
            )
            .await?;
        } else {
            self.notify(
                "textDocument/didChange",
                json!({
                    "textDocument": { "uri": uri, "version": version },
                    "contentChanges": [{ "text": text }],
                }),
            )
            .await?;
        }

        self.wait_for_diagnostics(&uri).await;
        Ok(uri)
    }

    async fn wait_for_diagnostics(&self, uri: &str) {
        let wait_ms = self.config.diag_wait_ms;
        let steps = (wait_ms / 50).max(1);
        for _ in 0..steps {
            if self.diagnostics.lock().await.contains_key(uri) {
                return;
            }
            sleep(Duration::from_millis(50)).await;
        }
        sleep(Duration::from_millis(wait_ms % 50)).await;
    }

    pub async fn diagnostics(&self, file_path: &str, refresh: bool) -> Result<Value> {
        let uri = if refresh {
            self.ensure_document_open(file_path).await?
        } else {
            crate::util::path_to_uri(&resolve_res_path(&self.config.project_path, file_path)?)
        };

        let diagnostics = self.diagnostics.lock().await;
        Ok(diagnostics
            .get(&uri)
            .cloned()
            .unwrap_or_else(|| json!({ "uri": uri, "diagnostics": [] })))
    }

    pub async fn hover(&self, file_path: &str, line: u32, character: u32) -> Result<Value> {
        let uri = self.ensure_document_open(file_path).await?;
        self.request(
            "textDocument/hover",
            json!({
                "textDocument": { "uri": uri },
                "position": { "line": line, "character": character },
            }),
        )
        .await
    }

    pub async fn definition(&self, file_path: &str, line: u32, character: u32) -> Result<Value> {
        let uri = self.ensure_document_open(file_path).await?;
        self.request(
            "textDocument/definition",
            json!({
                "textDocument": { "uri": uri },
                "position": { "line": line, "character": character },
            }),
        )
        .await
    }

    pub async fn completion(&self, file_path: &str, line: u32, character: u32) -> Result<Value> {
        let uri = self.ensure_document_open(file_path).await?;
        self.request(
            "textDocument/completion",
            json!({
                "textDocument": { "uri": uri },
                "position": { "line": line, "character": character },
            }),
        )
        .await
    }

    pub async fn document_symbols(&self, file_path: &str) -> Result<Value> {
        let uri = self.ensure_document_open(file_path).await?;
        self.request(
            "textDocument/documentSymbol",
            json!({ "textDocument": { "uri": uri } }),
        )
        .await
    }

    pub async fn references(&self, file_path: &str, line: u32, character: u32) -> Result<Value> {
        let uri = self.ensure_document_open(file_path).await?;
        self.request(
            "textDocument/references",
            json!({
                "textDocument": { "uri": uri },
                "position": { "line": line, "character": character },
                "context": { "includeDeclaration": true },
            }),
        )
        .await
    }

    pub async fn workspace_symbols(&self, query: &str) -> Result<Value> {
        self.ensure_connected().await?;
        self.request("workspace/symbol", json!({ "query": query }))
            .await
    }

    async fn ensure_connected(&self) -> Result<()> {
        if *self.initialized.lock().await {
            if self.write_tx.lock().await.is_some() {
                return Ok(());
            }
            *self.initialized.lock().await = false;
        }
        self.connect_and_initialize().await
    }

    async fn connect_and_initialize(&self) -> Result<()> {
        let addr = format!("{}:{}", self.config.host, self.config.port);
        let stream = TcpStream::connect(&addr)
            .await
            .with_context(|| format!("connect to Godot LSP at {addr}"))?;

        let (mut reader, mut writer) = stream.into_split();
        let (write_tx, mut write_rx) = mpsc::channel::<String>(32);

        tokio::spawn(async move {
            while let Some(frame) = write_rx.recv().await {
                if writer.write_all(frame.as_bytes()).await.is_err() {
                    break;
                }
                if writer.flush().await.is_err() {
                    break;
                }
            }
        });

        let pending = Arc::clone(&self.pending);
        let diagnostics = Arc::clone(&self.diagnostics);
        let initialized = Arc::clone(&self.initialized);
        tokio::spawn(async move {
            let mut read_buf = Vec::new();
            let mut scratch = [0u8; 8192];
            loop {
                match reader.read(&mut scratch).await {
                    Ok(0) => break,
                    Ok(n) => read_buf.extend_from_slice(&scratch[..n]),
                    Err(err) => {
                        tracing::warn!("LSP read error: {err}");
                        break;
                    }
                }

                while let Ok(Some(message)) = parse_message(&mut read_buf) {
                    dispatch_message(&pending, &diagnostics, message).await;
                }
            }

            *initialized.lock().await = false;
        });

        *self.write_tx.lock().await = Some(write_tx);
        self.pending.lock().await.clear();
        self.diagnostics.lock().await.clear();

        let result = self
            .request(
                "initialize",
                json!({
                    "processId": std::process::id(),
                    "rootUri": self.config.root_uri(),
                    "capabilities": {
                        "textDocument": {
                            "publishDiagnostics": {},
                            "hover": {},
                            "definition": {},
                            "completion": {},
                            "documentSymbol": {},
                            "references": {},
                        },
                        "workspace": {
                            "symbol": {},
                        }
                    },
                }),
            )
            .await?;

        tracing::debug!(?result, "LSP initialize result");
        self.notify("initialized", json!({})).await?;
        *self.initialized.lock().await = true;
        Ok(())
    }

    async fn request(&self, method: &str, params: Value) -> Result<Value> {
        static NEXT_ID: AtomicI64 = AtomicI64::new(1);
        let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);

        let (tx, rx) = oneshot::channel();
        self.pending.lock().await.insert(id, tx);
        self.send_message(&json!({
            "jsonrpc": "2.0",
            "id": id,
            "method": method,
            "params": params,
        }))
        .await?;

        let timeout = Duration::from_secs(self.config.request_timeout_secs);
        match tokio::time::timeout(timeout, rx).await {
            Ok(Ok(msg)) => {
                if let Some(err) = msg.get("error") {
                    Err(anyhow!("LSP error for {method}: {err}"))
                } else {
                    Ok(msg.get("result").cloned().unwrap_or(Value::Null))
                }
            }
            Ok(Err(_)) => Err(anyhow!("LSP response channel closed for {method}")),
            Err(_) => Err(anyhow!("LSP request timed out: {method}")),
        }
    }

    async fn notify(&self, method: &str, params: Value) -> Result<()> {
        self.send_message(&json!({
            "jsonrpc": "2.0",
            "method": method,
            "params": params,
        }))
        .await
    }

    async fn send_message(&self, message: &Value) -> Result<()> {
        let body = serde_json::to_string(message)?;
        let frame = format!("Content-Length: {}\r\n\r\n{}", body.len(), body);
        let tx = self
            .write_tx
            .lock()
            .await
            .as_ref()
            .ok_or_else(|| anyhow!("not connected to Godot LSP"))?
            .clone();
        tx.send(frame)
            .await
            .map_err(|_| anyhow!("LSP writer task stopped"))?;
        Ok(())
    }
}

async fn dispatch_message(
    pending: &Arc<Mutex<HashMap<i64, oneshot::Sender<Value>>>>,
    diagnostics: &Arc<Mutex<HashMap<String, Value>>>,
    message: Value,
) {
    if let Some(id) = message.get("id").and_then(Value::as_i64) {
        if let Some(tx) = pending.lock().await.remove(&id) {
            let _ = tx.send(message);
        }
        return;
    }

    let method = message
        .get("method")
        .and_then(Value::as_str)
        .unwrap_or_default();

    match method {
        "textDocument/publishDiagnostics" => {
            if let Some(params) = message.get("params") {
                if let Some(uri) = params.get("uri").and_then(Value::as_str) {
                    diagnostics
                        .lock()
                        .await
                        .insert(uri.to_string(), params.clone());
                }
            }
        }
        "gdscript_client/changeWorkspace" | "gdscript/capabilities" => {}
        other if !other.is_empty() => tracing::debug!(method = other, "LSP notification"),
        _ => {}
    }
}

fn parse_message(buf: &mut Vec<u8>) -> Result<Option<Value>> {
    let Some(header_end) = buf.windows(4).position(|w| w == b"\r\n\r\n") else {
        return Ok(None);
    };

    let header = std::str::from_utf8(&buf[..header_end]).context("invalid LSP header")?;
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
    let message: Value = serde_json::from_slice(body).context("invalid LSP JSON body")?;
    buf.drain(..body_start + content_length);
    Ok(Some(message))
}
