@tool
extends Node

const RpcHandlersScript = preload("res://addons/godot_mcp_bridge/rpc_handlers.gd")

const DEFAULT_PORT := 6007
const READ_BUFFER_SIZE := 65536

var _tcp: TCPServer
var _clients: Array[StreamPeerTCP] = []
var _read_buffers: Dictionary = {}
var _handlers: RefCounted
var _port: int = DEFAULT_PORT


func _ready() -> void:
	_handlers = RpcHandlersScript.new()


func start() -> void:
	if _tcp != null:
		return
	_port = int(OS.get_environment("GODOT_EDITOR_RPC_PORT") if OS.has_environment("GODOT_EDITOR_RPC_PORT") else DEFAULT_PORT)
	_tcp = TCPServer.new()
	var err := _tcp.listen(_port, "127.0.0.1")
	if err != OK:
		push_error("Godot MCP Bridge: failed to listen on port %d: %s" % [_port, error_string(err)])
		_tcp = null
	else:
		print("Godot MCP Bridge listening on 127.0.0.1:%d" % _port)


func stop() -> void:
	if _tcp:
		_tcp.stop()
		_tcp = null
	_clients.clear()
	_read_buffers.clear()


func _process(_delta: float) -> void:
	if _tcp == null:
		return

	while _tcp.is_connection_available():
		var peer: StreamPeerTCP = _tcp.take_connection()
		var addr := peer.get_connected_host()
		if addr != "127.0.0.1" and addr != "::1" and addr != "0:0:0:0:0:0:0:1":
			peer.disconnect_from_host()
			continue
		_clients.append(peer)
		_read_buffers[peer.get_instance_id()] = PackedByteArray()

	var i := _clients.size() - 1
	while i >= 0:
		var peer: StreamPeerTCP = _clients[i]
		if peer.get_status() != StreamPeerTCP.STATUS_CONNECTED:
			_read_buffers.erase(peer.get_instance_id())
			_clients.remove_at(i)
			i -= 1
			continue

		var available := peer.get_available_bytes()
		if available > 0:
			var chunk: PackedByteArray = peer.get_data(available)[1]
			var key := peer.get_instance_id()
			var buf: PackedByteArray = _read_buffers.get(key, PackedByteArray())
			buf.append_array(chunk)
			_read_buffers[key] = buf
			_dispatch_buffered(peer, key)

		i -= 1


func _dispatch_buffered(peer: StreamPeerTCP, key: int) -> void:
	var buf: PackedByteArray = _read_buffers.get(key, PackedByteArray())
	while true:
		var parsed := _try_parse_message(buf)
		if parsed.is_empty():
			_read_buffers[key] = buf
			return
		var message: Dictionary = parsed.get("message", {})
		buf = parsed.get("remaining", PackedByteArray())
		var response := _handle_message(message)
		if not response.is_empty():
			_send(peer, response)


func _try_parse_message(buf: PackedByteArray) -> Dictionary:
	var text := buf.get_string_from_utf8()
	var header_end := text.find("\r\n\r\n")
	if header_end == -1:
		return {}

	var header := text.substr(0, header_end)
	var content_length := -1
	for line in header.split("\r\n", false):
		if line.begins_with("Content-Length:"):
			content_length = int(line.substr("Content-Length:".length()).strip_edges())
			break
	if content_length < 0:
		return {}

	var body_start := header_end + 4
	if text.length() < body_start + content_length:
		return {}

	var body := text.substr(body_start, content_length)
	var remaining_text := text.substr(body_start + content_length)
	var remaining := remaining_text.to_utf8_buffer()

	var json := JSON.new()
	if json.parse(body) != OK:
		return { "message": {}, "remaining": remaining }

	return { "message": json.get_data(), "remaining": remaining }


func _handle_message(message: Variant) -> Dictionary:
	if typeof(message) != TYPE_DICTIONARY:
		return _error_response(null, -32600, "Invalid Request")

	var id = message.get("id", null)
	var method: String = str(message.get("method", ""))
	var params: Dictionary = message.get("params", {})

	if method.is_empty():
		return _error_response(id, -32600, "Missing method")

	var result = _handlers.call_rpc(method, params)
	if typeof(result) == TYPE_DICTIONARY and result.get("rpc_error", false):
		return _error_response(id, int(result.get("code", -32000)), str(result.get("message", "error")))

	return { "jsonrpc": "2.0", "id": id, "result": result }


func _error_response(id: Variant, code: int, message: String) -> Dictionary:
	return {
		"jsonrpc": "2.0",
		"id": id,
		"error": { "code": code, "message": message },
	}


func _send(peer: StreamPeerTCP, payload: Dictionary) -> void:
	var body := JSON.stringify(payload)
	var frame := "Content-Length: %d\r\n\r\n%s" % [body.to_utf8_buffer().size(), body]
	peer.put_data(frame.to_utf8_buffer())
