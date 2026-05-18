@tool
extends RefCounted

var _output_log: PackedStringArray = []


func call_rpc(method: String, params: Dictionary) -> Variant:
	match method:
		"editor.status":
			return _status()
		"editor.get_open_scenes":
			return _get_open_scenes()
		"editor.get_current_scene":
			return _get_current_scene()
		"editor.get_selection":
			return _get_selection()
		"editor.open_scene":
			return _open_scene(str(params.get("path", "")))
		"editor.save_scene":
			return _save_scene()
		"editor.play_scene":
			return _play_scene(params.get("path"))
		"editor.stop_play":
			return _stop_play()
		"editor.get_scene_tree":
			return _get_scene_tree()
		"editor.get_node_properties":
			return _get_node_properties(params.get("node_path"))
		"editor.inspect_resource":
			return _inspect_resource(str(params.get("path", "")))
		"editor.get_output_log":
			return { "lines": _output_log.slice(-200) }
		"editor.capture_screenshot":
			return _capture_screenshot(params.get("output_path"))
		"editor.get_runtime_scene_tree":
			return _get_runtime_scene_tree()
		"editor.simulate_input":
			return _simulate_input(str(params.get("action", "")), bool(params.get("pressed", true)))
		_:
			return { "rpc_error": true, "code": -32601, "message": "Method not found: %s" % method }


func _status() -> Dictionary:
	return {
		"alive": true,
		"godot_version": Engine.get_version_info(),
		"project_path": ProjectSettings.globalize_path("res://"),
		"is_playing": EditorInterface.is_playing_scene(),
	}


func _get_open_scenes() -> Dictionary:
	var scenes: Array[String] = []
	for path in EditorInterface.get_open_scenes():
		scenes.append(path)
	return { "scenes": scenes }


func _get_current_scene() -> Dictionary:
	var root := EditorInterface.get_edited_scene_root()
	if root == null:
		return { "path": null, "root": null }
	var scene_path := root.scene_file_path
	return {
		"path": scene_path if not scene_path.is_empty() else null,
		"root": _node_summary(root),
	}


func _get_selection() -> Dictionary:
	var nodes: Array = []
	for node in EditorInterface.get_selection().get_selected_nodes():
		nodes.append(_node_summary(node))
	return { "nodes": nodes }


func _open_scene(path: String) -> Dictionary:
	if path.is_empty():
		return { "rpc_error": true, "code": -32602, "message": "path required" }
	if not ResourceLoader.exists(path):
		return { "rpc_error": true, "code": -32000, "message": "scene not found: %s" % path }
	EditorInterface.open_scene_from_path(path)
	return { "ok": true, "path": path }


func _save_scene() -> Dictionary:
	var root := EditorInterface.get_edited_scene_root()
	if root == null:
		return { "rpc_error": true, "code": -32000, "message": "no scene open" }
	var path := root.scene_file_path
	var err := EditorInterface.save_scene()
	return { "ok": err == OK, "path": path, "error": error_string(err) if err != OK else "" }


func _play_scene(path: Variant) -> Dictionary:
	if path != null and str(path) != "":
		var scene_path := str(path)
		if not ResourceLoader.exists(scene_path):
			return { "rpc_error": true, "code": -32000, "message": "scene not found: %s" % scene_path }
		EditorInterface.play_custom_scene(scene_path)
		return { "ok": true, "path": scene_path }
	EditorInterface.play_main_scene()
	return { "ok": true, "path": "main_scene" }


func _stop_play() -> Dictionary:
	EditorInterface.stop_playing_scene()
	return { "ok": true }


func _get_scene_tree() -> Dictionary:
	var root := EditorInterface.get_edited_scene_root()
	if root == null:
		return { "tree": null }
	return { "tree": _node_tree(root) }


func _get_node_properties(node_path: Variant) -> Dictionary:
	var node: Node = null
	if node_path != null and str(node_path) != "":
		var root := EditorInterface.get_edited_scene_root()
		if root:
			node = root.get_node_or_null(str(node_path))
	else:
		var sel := EditorInterface.get_selection().get_selected_nodes()
		if sel.size() > 0:
			node = sel[0]
	if node == null:
		return { "rpc_error": true, "code": -32000, "message": "node not found" }
	return { "node": _node_summary(node), "properties": _serialize_properties(node) }


func _inspect_resource(path: String) -> Dictionary:
	if path.is_empty():
		return { "rpc_error": true, "code": -32602, "message": "path required" }
	if not ResourceLoader.exists(path):
		return { "rpc_error": true, "code": -32000, "message": "resource not found: %s" % path }
	var res: Resource = load(path)
	if res == null:
		return { "rpc_error": true, "code": -32000, "message": "failed to load: %s" % path }
	return {
		"path": path,
		"type": res.get_class(),
		"resource_path": res.resource_path,
		"properties": _serialize_properties(res),
	}


func _capture_screenshot(output_path: Variant) -> Dictionary:
	var path := str(output_path) if output_path != null else ""
	if path.is_empty():
		path = "user://mcp_screenshot_%d.png" % Time.get_unix_time_from_system()

	var vp := EditorInterface.get_editor_viewport_3d(0)
	if vp == null:
		return { "rpc_error": true, "code": -32000, "message": "no 3D editor viewport" }

	var tex: ViewportTexture = vp.get_texture()
	var img: Image = tex.get_image()
	if img == null or img.is_empty():
		return { "rpc_error": true, "code": -32000, "message": "empty viewport image" }

	var err := img.save_png(path)
	return { "ok": err == OK, "path": ProjectSettings.globalize_path(path) }


func _get_runtime_scene_tree() -> Dictionary:
	if not EditorInterface.is_playing_scene():
		return { "rpc_error": true, "code": -32000, "message": "not in play mode" }
	var tree := Engine.get_main_loop() as SceneTree
	if tree == null:
		return { "tree": null }
	var root := tree.current_scene
	if root == null:
		root = tree.root
	return { "tree": _node_tree(root) }


func _simulate_input(action: String, pressed: bool) -> Dictionary:
	if not EditorInterface.is_playing_scene():
		return { "rpc_error": true, "code": -32000, "message": "not in play mode" }
	if action.is_empty():
		return { "rpc_error": true, "code": -32602, "message": "action required" }
	if not InputMap.has_action(action):
		return { "rpc_error": true, "code": -32000, "message": "unknown action: %s" % action }

	var events := InputMap.action_get_events(action)
	if events.is_empty():
		return { "rpc_error": true, "code": -32000, "message": "no events for action" }

	var ev: InputEvent = events[0].duplicate()
	ev.set_pressed(pressed)
	Input.parse_input_event(ev)
	return { "ok": true, "action": action, "pressed": pressed }


func _node_summary(node: Node) -> Dictionary:
	var script_path := ""
	if node.get_script():
		script_path = str(node.get_script().resource_path)
	return {
		"name": node.name,
		"type": node.get_class(),
		"path": str(node.get_path()),
		"script": script_path,
	}


func _node_tree(node: Node) -> Dictionary:
	var children: Array = []
	for child in node.get_children():
		children.append(_node_tree(child))
	return {
		"name": node.name,
		"type": node.get_class(),
		"path": str(node.get_path()),
		"children": children,
	}


func _serialize_properties(obj: Object) -> Dictionary:
	var out := {}
	if obj == null:
		return out
	for prop in obj.get_property_list():
		var name: String = prop.get("name", "")
		if name.is_empty() or name.begins_with("_"):
			continue
		if (prop.get("usage", 0) & PROPERTY_USAGE_EDITOR) == 0:
			continue
		var value = obj.get(name)
		if typeof(value) in [TYPE_OBJECT, TYPE_RID]:
			continue
		if typeof(value) == TYPE_ARRAY and value.size() > 32:
			out[name] = "<array len=%d>" % value.size()
			continue
		if typeof(value) == TYPE_PACKED_BYTE_ARRAY and value.size() > 64:
			out[name] = "<packed bytes len=%d>" % value.size()
			continue
		out[name] = value
	return out
