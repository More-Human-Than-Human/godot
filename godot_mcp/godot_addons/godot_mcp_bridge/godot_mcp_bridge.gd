@tool
extends EditorPlugin

const RpcServerScript = preload("res://addons/godot_mcp_bridge/rpc_server.gd")

var _server: Node


func _enter_tree() -> void:
	_server = RpcServerScript.new()
	add_child(_server)
	_server.start()


func _exit_tree() -> void:
	if _server:
		_server.stop()
		_server.queue_free()
		_server = null
