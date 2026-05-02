/**************************************************************************/
/*  performance.cpp                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "performance.h"
#include "performance.compat.inc"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/variant/typed_array.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "servers/audio/audio_server.h"
#include "servers/rendering/rendering_server.h"

#ifndef _3D_DISABLED
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#endif // _3D_DISABLED

#ifndef NAVIGATION_2D_DISABLED
#include "servers/navigation_2d/navigation_server_2d.h"
#endif // NAVIGATION_2D_DISABLED

#ifndef NAVIGATION_3D_DISABLED
#include "servers/navigation_3d/navigation_server_3d.h"
#endif // NAVIGATION_3D_DISABLED

#ifndef PHYSICS_2D_DISABLED
#include "servers/physics_2d/physics_server_2d.h"
#endif // PHYSICS_2D_DISABLED

#ifndef PHYSICS_3D_DISABLED
#include "servers/physics_3d/physics_server_3d.h"
#endif // PHYSICS_3D_DISABLED

Performance *Performance::singleton = nullptr;

void Performance::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_monitor", "monitor"), &Performance::get_monitor);
	ClassDB::bind_method(D_METHOD("add_custom_monitor", "id", "callable", "arguments", "type"), &Performance::add_custom_monitor, DEFVAL(Array()), DEFVAL(MONITOR_TYPE_QUANTITY));
	ClassDB::bind_method(D_METHOD("remove_custom_monitor", "id"), &Performance::remove_custom_monitor);
	ClassDB::bind_method(D_METHOD("has_custom_monitor", "id"), &Performance::has_custom_monitor);
	ClassDB::bind_method(D_METHOD("get_custom_monitor", "id"), &Performance::get_custom_monitor);
	ClassDB::bind_method(D_METHOD("get_monitor_modification_time"), &Performance::get_monitor_modification_time);
	ClassDB::bind_method(D_METHOD("get_custom_monitor_names"), &Performance::get_custom_monitor_names);
	ClassDB::bind_method(D_METHOD("get_custom_monitor_types"), &Performance::get_custom_monitor_types);
	ClassDB::bind_method(D_METHOD("get_render_profile_stage_breakdown", "limit", "sort_by_gpu_total"), &Performance::get_render_profile_stage_breakdown, DEFVAL(8), DEFVAL(true));
	// DUMBAI: Bind workload snapshot accessor so GDScript benchmark runners can log pass-level pressure without adding renderer-specific script hacks.
	ClassDB::bind_method(D_METHOD("get_render_forward_clustered_workload_snapshot"), &Performance::get_render_forward_clustered_workload_snapshot);

	BIND_ENUM_CONSTANT(TIME_FPS);
	BIND_ENUM_CONSTANT(TIME_PROCESS);
	BIND_ENUM_CONSTANT(TIME_PHYSICS_PROCESS);
	BIND_ENUM_CONSTANT(TIME_NAVIGATION_PROCESS);
	BIND_ENUM_CONSTANT(MEMORY_STATIC);
	BIND_ENUM_CONSTANT(MEMORY_STATIC_MAX);
	BIND_ENUM_CONSTANT(MEMORY_MESSAGE_BUFFER_MAX);
	BIND_ENUM_CONSTANT(OBJECT_COUNT);
	BIND_ENUM_CONSTANT(OBJECT_RESOURCE_COUNT);
	BIND_ENUM_CONSTANT(OBJECT_NODE_COUNT);
	BIND_ENUM_CONSTANT(OBJECT_ORPHAN_NODE_COUNT);
	BIND_ENUM_CONSTANT(RENDER_TOTAL_OBJECTS_IN_FRAME);
	BIND_ENUM_CONSTANT(RENDER_TOTAL_PRIMITIVES_IN_FRAME);
	BIND_ENUM_CONSTANT(RENDER_TOTAL_DRAW_CALLS_IN_FRAME);
	BIND_ENUM_CONSTANT(RENDER_VIDEO_MEM_USED);
	BIND_ENUM_CONSTANT(RENDER_TEXTURE_MEM_USED);
	BIND_ENUM_CONSTANT(RENDER_BUFFER_MEM_USED);
	BIND_ENUM_CONSTANT(PHYSICS_2D_ACTIVE_OBJECTS);
	BIND_ENUM_CONSTANT(PHYSICS_2D_COLLISION_PAIRS);
	BIND_ENUM_CONSTANT(PHYSICS_2D_ISLAND_COUNT);
#ifndef _3D_DISABLED
	BIND_ENUM_CONSTANT(PHYSICS_3D_ACTIVE_OBJECTS);
	BIND_ENUM_CONSTANT(PHYSICS_3D_COLLISION_PAIRS);
	BIND_ENUM_CONSTANT(PHYSICS_3D_ISLAND_COUNT);
#endif // _3D_DISABLED
	BIND_ENUM_CONSTANT(AUDIO_OUTPUT_LATENCY);
#if !defined(NAVIGATION_2D_DISABLED) || !defined(NAVIGATION_3D_DISABLED)
	BIND_ENUM_CONSTANT(NAVIGATION_ACTIVE_MAPS);
	BIND_ENUM_CONSTANT(NAVIGATION_REGION_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_AGENT_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_LINK_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_POLYGON_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_EDGE_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_EDGE_MERGE_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_EDGE_CONNECTION_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_EDGE_FREE_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_OBSTACLE_COUNT);
#endif // !defined(NAVIGATION_2D_DISABLED) || !defined(NAVIGATION_3D_DISABLED)
	BIND_ENUM_CONSTANT(PIPELINE_COMPILATIONS_CANVAS);
	BIND_ENUM_CONSTANT(PIPELINE_COMPILATIONS_MESH);
	BIND_ENUM_CONSTANT(PIPELINE_COMPILATIONS_SURFACE);
	BIND_ENUM_CONSTANT(PIPELINE_COMPILATIONS_DRAW);
	BIND_ENUM_CONSTANT(PIPELINE_COMPILATIONS_SPECIALIZATION);
#ifndef NAVIGATION_2D_DISABLED
	BIND_ENUM_CONSTANT(NAVIGATION_2D_ACTIVE_MAPS);
	BIND_ENUM_CONSTANT(NAVIGATION_2D_REGION_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_2D_AGENT_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_2D_LINK_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_2D_POLYGON_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_2D_EDGE_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_2D_EDGE_MERGE_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_2D_EDGE_CONNECTION_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_2D_EDGE_FREE_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_2D_OBSTACLE_COUNT);
#endif // NAVIGATION_2D_DISABLED
#ifndef NAVIGATION_3D_DISABLED
	BIND_ENUM_CONSTANT(NAVIGATION_3D_ACTIVE_MAPS);
	BIND_ENUM_CONSTANT(NAVIGATION_3D_REGION_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_3D_AGENT_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_3D_LINK_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_3D_POLYGON_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_3D_EDGE_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_3D_EDGE_MERGE_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_3D_EDGE_CONNECTION_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_3D_EDGE_FREE_COUNT);
	BIND_ENUM_CONSTANT(NAVIGATION_3D_OBSTACLE_COUNT);
#endif // NAVIGATION_3D_DISABLED
	// DUMBAI: Expose idle-frame process sub-stage metrics to isolate whether script, queue, navigation, sync, or draw dominates.
	BIND_ENUM_CONSTANT(TIME_PROCESS_MAIN_LOOP);
	BIND_ENUM_CONSTANT(TIME_PROCESS_MESSAGE_QUEUE);
	BIND_ENUM_CONSTANT(TIME_PROCESS_NAVIGATION_IDLE);
	BIND_ENUM_CONSTANT(TIME_PROCESS_RENDER_SYNC);
	BIND_ENUM_CONSTANT(TIME_PROCESS_RENDER_DRAW);
	// DUMBAI: Expose SceneTree::process sub-stages so benchmark exports can attribute _process cost to concrete engine phases.
	BIND_ENUM_CONSTANT(TIME_PROCESS_SCENE_TREE_FTI);
	BIND_ENUM_CONSTANT(TIME_PROCESS_SCENE_TREE_MAIN_LOOP);
	BIND_ENUM_CONSTANT(TIME_PROCESS_SCENE_TREE_MULTIPLAYER);
	BIND_ENUM_CONSTANT(TIME_PROCESS_SCENE_TREE_SIGNAL);
	BIND_ENUM_CONSTANT(TIME_PROCESS_SCENE_TREE_MESSAGE_QUEUE);
	BIND_ENUM_CONSTANT(TIME_PROCESS_SCENE_TREE_TRANSFORM_FLUSH);
	BIND_ENUM_CONSTANT(TIME_PROCESS_SCENE_TREE_GROUPS);
	BIND_ENUM_CONSTANT(TIME_PROCESS_SCENE_TREE_UGC);
	BIND_ENUM_CONSTANT(TIME_PROCESS_SCENE_TREE_SCENE_CHANGE);
	BIND_ENUM_CONSTANT(TIME_PROCESS_SCENE_TREE_TIMERS);
	BIND_ENUM_CONSTANT(TIME_PROCESS_SCENE_TREE_TWEENS);
	BIND_ENUM_CONSTANT(TIME_PROCESS_SCENE_TREE_DELETE_QUEUE);
	BIND_ENUM_CONSTANT(TIME_PROCESS_SCENE_TREE_ACCESSIBILITY);
	BIND_ENUM_CONSTANT(TIME_PROCESS_SCENE_TREE_IDLE_CALLBACKS);
	BIND_ENUM_CONSTANT(TIME_PROCESS_SCENE_TREE_OTHER);
	// DUMBAI: Expose renderer frame-profile CPU/GPU timings for spike attribution against the 16.67ms target budget.
	BIND_ENUM_CONSTANT(TIME_RENDER_FRAME_SETUP_CPU);
	BIND_ENUM_CONSTANT(TIME_RENDER_PROFILE_CPU_TOTAL);
	BIND_ENUM_CONSTANT(TIME_RENDER_PROFILE_GPU_TOTAL);
	BIND_ENUM_CONSTANT(TIME_RENDER_PROFILE_CPU_MAX_STAGE);
	BIND_ENUM_CONSTANT(TIME_RENDER_PROFILE_GPU_MAX_STAGE);
	BIND_ENUM_CONSTANT(TIME_RENDER_PROFILE_STAGE_COUNT);
	BIND_ENUM_CONSTANT(TIME_RENDER_PROFILE_GPU_NONZERO_STAGE_COUNT);
	BIND_ENUM_CONSTANT(TIME_RENDER_PROFILE_GPU_ZERO_STAGE_COUNT);
	BIND_ENUM_CONSTANT(MONITOR_MAX);

	BIND_ENUM_CONSTANT(MONITOR_TYPE_QUANTITY);
	BIND_ENUM_CONSTANT(MONITOR_TYPE_MEMORY);
	BIND_ENUM_CONSTANT(MONITOR_TYPE_TIME);
	BIND_ENUM_CONSTANT(MONITOR_TYPE_PERCENTAGE);
}

int Performance::_get_node_count() const {
	MainLoop *ml = OS::get_singleton()->get_main_loop();
	SceneTree *sml = Object::cast_to<SceneTree>(ml);
	if (!sml) {
		return 0;
	}
	return sml->get_node_count();
}

int Performance::_get_orphan_node_count() const {
#ifdef DEBUG_ENABLED
	const int total_node_count = Node::total_node_count.get();
	const int orphan_node_count = total_node_count - _get_node_count();
	return orphan_node_count;
#else
	return 0;
#endif
}

String Performance::get_monitor_name(Monitor p_monitor) const {
	ERR_FAIL_INDEX_V(p_monitor, MONITOR_MAX, String());
	static const char *names[MONITOR_MAX] = {
		PNAME("time/fps"),
		PNAME("time/process"),
		PNAME("time/physics_process"),
		PNAME("time/navigation_process"),
		PNAME("memory/static"),
		PNAME("memory/static_max"),
		PNAME("memory/msg_buf_max"),
		PNAME("object/objects"),
		PNAME("object/resources"),
		PNAME("object/nodes"),
		PNAME("object/orphan_nodes"),
		PNAME("raster/total_objects_drawn"),
		PNAME("raster/total_primitives_drawn"),
		PNAME("raster/total_draw_calls"),
		PNAME("video/video_mem"),
		PNAME("video/texture_mem"),
		PNAME("video/buffer_mem"),
		PNAME("physics_2d/active_objects"),
		PNAME("physics_2d/collision_pairs"),
		PNAME("physics_2d/islands"),
		PNAME("physics_3d/active_objects"),
		PNAME("physics_3d/collision_pairs"),
		PNAME("physics_3d/islands"),
		PNAME("audio/driver/output_latency"),
#if !defined(NAVIGATION_2D_DISABLED) || !defined(NAVIGATION_3D_DISABLED)
		PNAME("navigation/active_maps"),
		PNAME("navigation/regions"),
		PNAME("navigation/agents"),
		PNAME("navigation/links"),
		PNAME("navigation/polygons"),
		PNAME("navigation/edges"),
		PNAME("navigation/edges_merged"),
		PNAME("navigation/edges_connected"),
		PNAME("navigation/edges_free"),
		PNAME("navigation/obstacles"),
#endif // !defined(NAVIGATION_2D_DISABLED) || !defined(NAVIGATION_3D_DISABLED)
		PNAME("pipeline/compilations_canvas"),
		PNAME("pipeline/compilations_mesh"),
		PNAME("pipeline/compilations_surface"),
		PNAME("pipeline/compilations_draw"),
		PNAME("pipeline/compilations_specialization"),
#ifndef NAVIGATION_2D_DISABLED
		PNAME("navigation_2d/active_maps"),
		PNAME("navigation_2d/regions"),
		PNAME("navigation_2d/agents"),
		PNAME("navigation_2d/links"),
		PNAME("navigation_2d/polygons"),
		PNAME("navigation_2d/edges"),
		PNAME("navigation_2d/edges_merged"),
		PNAME("navigation_2d/edges_connected"),
		PNAME("navigation_2d/edges_free"),
		PNAME("navigation_2d/obstacles"),
#endif // NAVIGATION_2D_DISABLED
#ifndef NAVIGATION_3D_DISABLED
		PNAME("navigation_3d/active_maps"),
		PNAME("navigation_3d/regions"),
		PNAME("navigation_3d/agents"),
		PNAME("navigation_3d/links"),
		PNAME("navigation_3d/polygons"),
		PNAME("navigation_3d/edges"),
		PNAME("navigation_3d/edges_merged"),
		PNAME("navigation_3d/edges_connected"),
		PNAME("navigation_3d/edges_free"),
		PNAME("navigation_3d/obstacles"),
#endif // NAVIGATION_3D_DISABLED
		PNAME("time/process_main_loop"),
		PNAME("time/process_message_queue"),
		PNAME("time/process_navigation_idle"),
		PNAME("time/process_render_sync"),
		PNAME("time/process_render_draw"),
		// DUMBAI: Keep monitor names flat for CSV exports and scripted lookups in benchmark automation.
		PNAME("time/process_scene_tree_fti"),
		PNAME("time/process_scene_tree_main_loop"),
		PNAME("time/process_scene_tree_multiplayer"),
		PNAME("time/process_scene_tree_signal"),
		PNAME("time/process_scene_tree_message_queue"),
		PNAME("time/process_scene_tree_transform_flush"),
		PNAME("time/process_scene_tree_groups"),
		PNAME("time/process_scene_tree_ugc"),
		PNAME("time/process_scene_tree_scene_change"),
		PNAME("time/process_scene_tree_timers"),
		PNAME("time/process_scene_tree_tweens"),
		PNAME("time/process_scene_tree_delete_queue"),
		PNAME("time/process_scene_tree_accessibility"),
		PNAME("time/process_scene_tree_idle_callbacks"),
		PNAME("time/process_scene_tree_other"),
		PNAME("time/render_frame_setup_cpu"),
		PNAME("time/render_profile_cpu_total"),
		PNAME("time/render_profile_gpu_total"),
		PNAME("time/render_profile_cpu_max_stage"),
		PNAME("time/render_profile_gpu_max_stage"),
		PNAME("time/render_profile_stage_count"),
		PNAME("time/render_profile_gpu_nonzero_stage_count"),
		PNAME("time/render_profile_gpu_zero_stage_count"),
	};
	static_assert(std_size(names) == MONITOR_MAX);

	return names[p_monitor];
}

double Performance::get_monitor(Monitor p_monitor) const {
	int info = 0;

	switch (p_monitor) {
		case TIME_FPS:
			return Engine::get_singleton()->get_frames_per_second();
		case TIME_PROCESS:
			return _process_time;
		case TIME_PHYSICS_PROCESS:
			return _physics_process_time;
		case TIME_NAVIGATION_PROCESS:
			return _navigation_process_time;
		case TIME_PROCESS_MAIN_LOOP:
			return _process_main_loop_time;
		case TIME_PROCESS_MESSAGE_QUEUE:
			return _process_message_queue_time;
		case TIME_PROCESS_NAVIGATION_IDLE:
			return _process_navigation_idle_time;
		case TIME_PROCESS_RENDER_SYNC:
			return _process_render_sync_time;
		case TIME_PROCESS_RENDER_DRAW:
			return _process_render_draw_time;
		// DUMBAI: Surface SceneTree idle-frame sub-stage durations as first-class monitors for automated bottleneck triage.
		case TIME_PROCESS_SCENE_TREE_FTI:
			return _process_scene_tree_fti_time;
		case TIME_PROCESS_SCENE_TREE_MAIN_LOOP:
			return _process_scene_tree_main_loop_time;
		case TIME_PROCESS_SCENE_TREE_MULTIPLAYER:
			return _process_scene_tree_multiplayer_time;
		case TIME_PROCESS_SCENE_TREE_SIGNAL:
			return _process_scene_tree_signal_time;
		case TIME_PROCESS_SCENE_TREE_MESSAGE_QUEUE:
			return _process_scene_tree_message_queue_time;
		case TIME_PROCESS_SCENE_TREE_TRANSFORM_FLUSH:
			return _process_scene_tree_transform_flush_time;
		case TIME_PROCESS_SCENE_TREE_GROUPS:
			return _process_scene_tree_groups_time;
		case TIME_PROCESS_SCENE_TREE_UGC:
			return _process_scene_tree_ugc_time;
		case TIME_PROCESS_SCENE_TREE_SCENE_CHANGE:
			return _process_scene_tree_scene_change_time;
		case TIME_PROCESS_SCENE_TREE_TIMERS:
			return _process_scene_tree_timers_time;
		case TIME_PROCESS_SCENE_TREE_TWEENS:
			return _process_scene_tree_tweens_time;
		case TIME_PROCESS_SCENE_TREE_DELETE_QUEUE:
			return _process_scene_tree_delete_queue_time;
		case TIME_PROCESS_SCENE_TREE_ACCESSIBILITY:
			return _process_scene_tree_accessibility_time;
		case TIME_PROCESS_SCENE_TREE_IDLE_CALLBACKS:
			return _process_scene_tree_idle_callbacks_time;
		case TIME_PROCESS_SCENE_TREE_OTHER:
			return _process_scene_tree_other_time;
		case TIME_RENDER_FRAME_SETUP_CPU:
			return RS::get_singleton()->get_frame_setup_time_cpu() / 1000.0;
		case TIME_RENDER_PROFILE_CPU_TOTAL:
			_refresh_render_profile_cache();
			return _render_profile_cpu_total_time;
		case TIME_RENDER_PROFILE_GPU_TOTAL:
			_refresh_render_profile_cache();
			return _render_profile_gpu_total_time;
		case TIME_RENDER_PROFILE_CPU_MAX_STAGE:
			_refresh_render_profile_cache();
			return _render_profile_cpu_max_stage_time;
		case TIME_RENDER_PROFILE_GPU_MAX_STAGE:
			_refresh_render_profile_cache();
			return _render_profile_gpu_max_stage_time;
		case TIME_RENDER_PROFILE_STAGE_COUNT:
			_refresh_render_profile_cache();
			return _render_profile_stage_count;
		case TIME_RENDER_PROFILE_GPU_NONZERO_STAGE_COUNT:
			_refresh_render_profile_cache();
			return _render_profile_gpu_nonzero_stage_count;
		case TIME_RENDER_PROFILE_GPU_ZERO_STAGE_COUNT:
			_refresh_render_profile_cache();
			return _render_profile_gpu_zero_stage_count;
		case MEMORY_STATIC:
			return Memory::get_mem_usage();
		case MEMORY_STATIC_MAX:
			return Memory::get_mem_max_usage();
		case MEMORY_MESSAGE_BUFFER_MAX:
			return MessageQueue::get_singleton()->get_max_buffer_usage();
		case OBJECT_COUNT:
			return ObjectDB::get_object_count();
		case OBJECT_RESOURCE_COUNT:
			return ResourceCache::get_cached_resource_count();
		case OBJECT_NODE_COUNT:
			return _get_node_count();
		case OBJECT_ORPHAN_NODE_COUNT:
			return _get_orphan_node_count();
		case RENDER_TOTAL_OBJECTS_IN_FRAME:
			return RS::get_singleton()->get_rendering_info(RSE::RENDERING_INFO_TOTAL_OBJECTS_IN_FRAME);
		case RENDER_TOTAL_PRIMITIVES_IN_FRAME:
			return RS::get_singleton()->get_rendering_info(RSE::RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME);
		case RENDER_TOTAL_DRAW_CALLS_IN_FRAME:
			return RS::get_singleton()->get_rendering_info(RSE::RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME);
		case RENDER_VIDEO_MEM_USED:
			return RS::get_singleton()->get_rendering_info(RSE::RENDERING_INFO_VIDEO_MEM_USED);
		case RENDER_TEXTURE_MEM_USED:
			return RS::get_singleton()->get_rendering_info(RSE::RENDERING_INFO_TEXTURE_MEM_USED);
		case RENDER_BUFFER_MEM_USED:
			return RS::get_singleton()->get_rendering_info(RSE::RENDERING_INFO_BUFFER_MEM_USED);
		case PIPELINE_COMPILATIONS_CANVAS:
			return RS::get_singleton()->get_rendering_info(RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_CANVAS);
		case PIPELINE_COMPILATIONS_MESH:
			return RS::get_singleton()->get_rendering_info(RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_MESH);
		case PIPELINE_COMPILATIONS_SURFACE:
			return RS::get_singleton()->get_rendering_info(RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_SURFACE);
		case PIPELINE_COMPILATIONS_DRAW:
			return RS::get_singleton()->get_rendering_info(RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_DRAW);
		case PIPELINE_COMPILATIONS_SPECIALIZATION:
			return RS::get_singleton()->get_rendering_info(RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_SPECIALIZATION);
#ifndef PHYSICS_2D_DISABLED
		case PHYSICS_2D_ACTIVE_OBJECTS:
			return PhysicsServer2D::get_singleton()->get_process_info(PS2DE::INFO_ACTIVE_OBJECTS);
		case PHYSICS_2D_COLLISION_PAIRS:
			return PhysicsServer2D::get_singleton()->get_process_info(PS2DE::INFO_COLLISION_PAIRS);
		case PHYSICS_2D_ISLAND_COUNT:
			return PhysicsServer2D::get_singleton()->get_process_info(PS2DE::INFO_ISLAND_COUNT);
#else
		case PHYSICS_2D_ACTIVE_OBJECTS:
			return 0;
		case PHYSICS_2D_COLLISION_PAIRS:
			return 0;
		case PHYSICS_2D_ISLAND_COUNT:
			return 0;
#endif // PHYSICS_2D_DISABLED
#ifndef PHYSICS_3D_DISABLED
		case PHYSICS_3D_ACTIVE_OBJECTS:
			return PhysicsServer3D::get_singleton()->get_process_info(PS3DE::INFO_ACTIVE_OBJECTS);
		case PHYSICS_3D_COLLISION_PAIRS:
			return PhysicsServer3D::get_singleton()->get_process_info(PS3DE::INFO_COLLISION_PAIRS);
		case PHYSICS_3D_ISLAND_COUNT:
			return PhysicsServer3D::get_singleton()->get_process_info(PS3DE::INFO_ISLAND_COUNT);
#else
		case PHYSICS_3D_ACTIVE_OBJECTS:
			return 0;
		case PHYSICS_3D_COLLISION_PAIRS:
			return 0;
		case PHYSICS_3D_ISLAND_COUNT:
			return 0;
#endif // PHYSICS_3D_DISABLED

		case AUDIO_OUTPUT_LATENCY:
			return AudioServer::get_singleton()->get_output_latency();

			// Deprecated, use the 2D/3D specific ones instead.
		case NAVIGATION_ACTIVE_MAPS:
#ifndef NAVIGATION_2D_DISABLED
			info = NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_ACTIVE_MAPS);
#endif // NAVIGATION_2D_DISABLED
#ifndef NAVIGATION_3D_DISABLED
			info += NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_ACTIVE_MAPS);
#endif // NAVIGATION_3D_DISABLED
			return info;

		case NAVIGATION_REGION_COUNT:
#ifndef NAVIGATION_2D_DISABLED
			info = NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_REGION_COUNT);
#endif // NAVIGATION_2D_DISABLED
#ifndef NAVIGATION_3D_DISABLED
			info += NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_REGION_COUNT);
#endif // NAVIGATION_3D_DISABLED
			return info;

		case NAVIGATION_AGENT_COUNT:
#ifndef NAVIGATION_2D_DISABLED
			info = NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_AGENT_COUNT);
#endif // NAVIGATION_2D_DISABLED
#ifndef NAVIGATION_3D_DISABLED
			info += NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_AGENT_COUNT);
#endif // NAVIGATION_3D_DISABLED
			return info;

		case NAVIGATION_LINK_COUNT:
#ifndef NAVIGATION_2D_DISABLED
			info = NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_LINK_COUNT);
#endif // NAVIGATION_2D_DISABLED
#ifndef NAVIGATION_3D_DISABLED
			info += NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_LINK_COUNT);
#endif // NAVIGATION_3D_DISABLED
			return info;

		case NAVIGATION_POLYGON_COUNT:
#ifndef NAVIGATION_2D_DISABLED
			info = NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_POLYGON_COUNT);
#endif // NAVIGATION_2D_DISABLED
#ifndef NAVIGATION_3D_DISABLED
			info += NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_POLYGON_COUNT);
#endif // NAVIGATION_3D_DISABLED
			return info;

		case NAVIGATION_EDGE_COUNT:
#ifndef NAVIGATION_2D_DISABLED
			info = NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_EDGE_COUNT);
#endif // NAVIGATION_2D_DISABLED
#ifndef NAVIGATION_3D_DISABLED
			info += NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_EDGE_COUNT);
#endif // NAVIGATION_3D_DISABLED
			return info;

		case NAVIGATION_EDGE_MERGE_COUNT:
#ifndef NAVIGATION_2D_DISABLED
			info = NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_EDGE_MERGE_COUNT);
#endif // NAVIGATION_2D_DISABLED
#ifndef NAVIGATION_3D_DISABLED
			info += NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_EDGE_MERGE_COUNT);
#endif // NAVIGATION_3D_DISABLED
			return info;

		case NAVIGATION_EDGE_CONNECTION_COUNT:
#ifndef NAVIGATION_2D_DISABLED
			info = NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_EDGE_CONNECTION_COUNT);
#endif // NAVIGATION_2D_DISABLED
#ifndef NAVIGATION_3D_DISABLED
			info += NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_EDGE_CONNECTION_COUNT);
#endif // NAVIGATION_3D_DISABLED
			return info;

		case NAVIGATION_EDGE_FREE_COUNT:
#ifndef NAVIGATION_2D_DISABLED
			info = NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_EDGE_FREE_COUNT);
#endif // NAVIGATION_2D_DISABLED
#ifndef NAVIGATION_3D_DISABLED
			info += NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_EDGE_FREE_COUNT);
#endif // NAVIGATION_3D_DISABLED
			return info;

		case NAVIGATION_OBSTACLE_COUNT:
#ifndef NAVIGATION_2D_DISABLED
			info = NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_OBSTACLE_COUNT);
#endif // NAVIGATION_2D_DISABLED
#ifndef NAVIGATION_3D_DISABLED
			info += NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_OBSTACLE_COUNT);
#endif // NAVIGATION_3D_DISABLED
			return info;

#ifndef NAVIGATION_2D_DISABLED
		case NAVIGATION_2D_ACTIVE_MAPS:
			return NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_ACTIVE_MAPS);
		case NAVIGATION_2D_REGION_COUNT:
			return NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_REGION_COUNT);
		case NAVIGATION_2D_AGENT_COUNT:
			return NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_AGENT_COUNT);
		case NAVIGATION_2D_LINK_COUNT:
			return NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_LINK_COUNT);
		case NAVIGATION_2D_POLYGON_COUNT:
			return NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_POLYGON_COUNT);
		case NAVIGATION_2D_EDGE_COUNT:
			return NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_EDGE_COUNT);
		case NAVIGATION_2D_EDGE_MERGE_COUNT:
			return NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_EDGE_MERGE_COUNT);
		case NAVIGATION_2D_EDGE_CONNECTION_COUNT:
			return NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_EDGE_CONNECTION_COUNT);
		case NAVIGATION_2D_EDGE_FREE_COUNT:
			return NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_EDGE_FREE_COUNT);
		case NAVIGATION_2D_OBSTACLE_COUNT:
			return NavigationServer2D::get_singleton()->get_process_info(NavigationServer2D::INFO_OBSTACLE_COUNT);
#endif // NAVIGATION_2D_DISABLED

#ifndef NAVIGATION_3D_DISABLED
		case NAVIGATION_3D_ACTIVE_MAPS:
			return NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_ACTIVE_MAPS);
		case NAVIGATION_3D_REGION_COUNT:
			return NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_REGION_COUNT);
		case NAVIGATION_3D_AGENT_COUNT:
			return NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_AGENT_COUNT);
		case NAVIGATION_3D_LINK_COUNT:
			return NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_LINK_COUNT);
		case NAVIGATION_3D_POLYGON_COUNT:
			return NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_POLYGON_COUNT);
		case NAVIGATION_3D_EDGE_COUNT:
			return NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_EDGE_COUNT);
		case NAVIGATION_3D_EDGE_MERGE_COUNT:
			return NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_EDGE_MERGE_COUNT);
		case NAVIGATION_3D_EDGE_CONNECTION_COUNT:
			return NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_EDGE_CONNECTION_COUNT);
		case NAVIGATION_3D_EDGE_FREE_COUNT:
			return NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_EDGE_FREE_COUNT);
		case NAVIGATION_3D_OBSTACLE_COUNT:
			return NavigationServer3D::get_singleton()->get_process_info(NavigationServer3D::INFO_OBSTACLE_COUNT);
#endif // NAVIGATION_3D_DISABLED

		default: {
		}
	}

	return 0;
}

void Performance::_ensure_render_profile_capture_enabled() const {
	if (_render_profile_capture_enabled) {
		return;
	}

	// DUMBAI: Enable renderer timestamp capture lazily so GPU profile overhead only appears when render profile monitors are queried.
	RS::get_singleton()->set_frame_profiling_enabled(true);
	_render_profile_capture_enabled = true;
}

void Performance::_refresh_render_profile_cache() const {
	_ensure_render_profile_capture_enabled();

	const uint64_t profile_frame = RS::get_singleton()->get_frame_profile_frame();
	if (_render_profile_cache_valid && _render_profile_cached_frame == profile_frame) {
		return;
	}

	_render_profile_cached_frame = profile_frame;
	_render_profile_cache_valid = true;
	_render_profile_cpu_total_time = 0.0;
	_render_profile_gpu_total_time = 0.0;
	_render_profile_cpu_max_stage_time = 0.0;
	_render_profile_gpu_max_stage_time = 0.0;
	_render_profile_stage_count = 0.0;
	_render_profile_gpu_nonzero_stage_count = 0.0;
	_render_profile_gpu_zero_stage_count = 0.0;

	const Vector<RenderingServerTypes::FrameProfileArea> frame_profile = RS::get_singleton()->get_frame_profile();
	if (frame_profile.is_empty()) {
		return;
	}

	const RenderingServerTypes::FrameProfileArea &frame_tail = frame_profile[frame_profile.size() - 1];
	_render_profile_cpu_total_time = frame_tail.cpu_msec / 1000.0;
	_render_profile_gpu_total_time = frame_tail.gpu_msec / 1000.0;

	for (int i = 0; i < frame_profile.size() - 1; i++) {
		const RenderingServerTypes::FrameProfileArea &area = frame_profile[i];
		if (area.name.is_empty()) {
			continue;
		}
		// DUMBAI: Skip internal bracket markers so max-stage timing reflects real render tasks only.
		if (area.name[0] == '<' || area.name[0] == '>') {
			continue;
		}

		const RenderingServerTypes::FrameProfileArea &next = frame_profile[i + 1];
		const double stage_cpu_time = MAX(0.0, next.cpu_msec - area.cpu_msec) / 1000.0;
		const double stage_gpu_time = MAX(0.0, next.gpu_msec - area.gpu_msec) / 1000.0;
		_render_profile_stage_count += 1.0;
		// DUMBAI: Split non-zero vs zero GPU stage samples to expose missing timestamp support in automated CSV runs.
		if (stage_gpu_time > 0.0) {
			_render_profile_gpu_nonzero_stage_count += 1.0;
		} else {
			_render_profile_gpu_zero_stage_count += 1.0;
		}
		_render_profile_cpu_max_stage_time = MAX(_render_profile_cpu_max_stage_time, stage_cpu_time);
		_render_profile_gpu_max_stage_time = MAX(_render_profile_gpu_max_stage_time, stage_gpu_time);
	}
}

Array Performance::get_render_profile_stage_breakdown(int p_limit, bool p_sort_by_gpu_total) const {
	_ensure_render_profile_capture_enabled();

	Array breakdown;
	const Vector<RenderingServerTypes::FrameProfileArea> frame_profile = RS::get_singleton()->get_frame_profile();
	if (frame_profile.size() < 2) {
		return breakdown;
	}

	struct StageTotals {
		double cpu_total_time = 0.0;
		double gpu_total_time = 0.0;
		double cpu_max_time = 0.0;
		double gpu_max_time = 0.0;
		int sample_count = 0;
	};

	HashMap<String, StageTotals> totals_by_stage;
	for (int i = 0; i < frame_profile.size() - 1; i++) {
		const RenderingServerTypes::FrameProfileArea &area = frame_profile[i];
		if (area.name.is_empty()) {
			continue;
		}
		// DUMBAI: Ignore profiler bracket markers so stage ranking reflects actionable renderer work items only.
		if (area.name[0] == '<' || area.name[0] == '>') {
			continue;
		}

		const RenderingServerTypes::FrameProfileArea &next = frame_profile[i + 1];
		const double stage_cpu_time = MAX(0.0, next.cpu_msec - area.cpu_msec) / 1000.0;
		const double stage_gpu_time = MAX(0.0, next.gpu_msec - area.gpu_msec) / 1000.0;
		const String stage_name = String(area.name);

		StageTotals *existing = totals_by_stage.getptr(stage_name);
		if (existing == nullptr) {
			StageTotals initial;
			initial.cpu_total_time = stage_cpu_time;
			initial.gpu_total_time = stage_gpu_time;
			initial.cpu_max_time = stage_cpu_time;
			initial.gpu_max_time = stage_gpu_time;
			initial.sample_count = 1;
			totals_by_stage.insert(stage_name, initial);
			continue;
		}

		existing->cpu_total_time += stage_cpu_time;
		existing->gpu_total_time += stage_gpu_time;
		existing->cpu_max_time = MAX(existing->cpu_max_time, stage_cpu_time);
		existing->gpu_max_time = MAX(existing->gpu_max_time, stage_gpu_time);
		existing->sample_count += 1;
	}

	if (totals_by_stage.is_empty()) {
		return breakdown;
	}

	Vector<String> stage_names;
	for (const KeyValue<String, StageTotals> &entry : totals_by_stage) {
		stage_names.push_back(entry.key);
	}

	const int stage_limit = CLAMP(p_limit, 1, stage_names.size());
	PackedByteArray used_indices;
	used_indices.resize(stage_names.size());
	uint8_t *used = used_indices.ptrw();
	for (int i = 0; i < stage_names.size(); i++) {
		used[i] = 0;
	}

	const RenderingServerTypes::FrameProfileArea &frame_tail = frame_profile[frame_profile.size() - 1];
	const double frame_cpu_total_time = MAX(0.0, frame_tail.cpu_msec) / 1000.0;
	const double frame_gpu_total_time = MAX(0.0, frame_tail.gpu_msec) / 1000.0;

	for (int rank = 0; rank < stage_limit; rank++) {
		int best_index = -1;
		double best_score = -1.0;
		for (int i = 0; i < stage_names.size(); i++) {
			if (used[i] != 0) {
				continue;
			}
			const StageTotals *candidate = totals_by_stage.getptr(stage_names[i]);
			if (candidate == nullptr) {
				continue;
			}
			const double score = p_sort_by_gpu_total ? candidate->gpu_total_time : candidate->cpu_total_time;
			if (score > best_score) {
				best_score = score;
				best_index = i;
			}
		}

		if (best_index < 0) {
			break;
		}
		used[best_index] = 1;

		const StageTotals *selected = totals_by_stage.getptr(stage_names[best_index]);
		if (selected == nullptr) {
			continue;
		}

		Dictionary stage_row;
		stage_row["name"] = stage_names[best_index];
		// DUMBAI: Export stage times in milliseconds to align directly with 16.67ms/frame optimization budgeting.
		stage_row["cpu_ms"] = selected->cpu_total_time * 1000.0;
		stage_row["gpu_ms"] = selected->gpu_total_time * 1000.0;
		stage_row["cpu_max_ms"] = selected->cpu_max_time * 1000.0;
		stage_row["gpu_max_ms"] = selected->gpu_max_time * 1000.0;
		stage_row["sample_count"] = selected->sample_count;
		// DUMBAI: Include frame-share ratios so top stages can be prioritized by contribution, not only absolute milliseconds.
		stage_row["cpu_frame_share"] = frame_cpu_total_time > 0.0 ? selected->cpu_total_time / frame_cpu_total_time : 0.0;
		stage_row["gpu_frame_share"] = frame_gpu_total_time > 0.0 ? selected->gpu_total_time / frame_gpu_total_time : 0.0;
		breakdown.push_back(stage_row);
	}

	return breakdown;
}

Dictionary Performance::get_render_forward_clustered_workload_snapshot() const {
#ifndef _3D_DISABLED
	RendererSceneRenderImplementation::RenderForwardClustered *forward_clustered = RendererSceneRenderImplementation::RenderForwardClustered::get_singleton();
	if (forward_clustered != nullptr) {
		// DUMBAI: Forward+ snapshot gives per-pass workload counts that explain why specific GPU stages dominate in benchmark traces.
		return forward_clustered->get_benchmark_workload_snapshot();
	}
#endif // _3D_DISABLED

	return Dictionary();
}

Performance::MonitorType Performance::get_monitor_type(Monitor p_monitor) const {
	ERR_FAIL_INDEX_V(p_monitor, MONITOR_MAX, MONITOR_TYPE_QUANTITY);
	// ugly
	static const MonitorType types[MONITOR_MAX] = {
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_MEMORY,
		MONITOR_TYPE_MEMORY,
		MONITOR_TYPE_MEMORY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_MEMORY,
		MONITOR_TYPE_MEMORY,
		MONITOR_TYPE_MEMORY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
#ifndef _3D_DISABLED
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
#endif // _3D_DISABLED
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		// DUMBAI: SceneTree::process sub-stage monitors are timing values in seconds.
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		// DUMBAI: Render profile/renderer setup monitors are also reported as time values (seconds).
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_TIME,
		// DUMBAI: Frame-profile sample health monitors are counts, not durations.
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_QUANTITY,
	};
	static_assert((sizeof(types) / sizeof(MonitorType)) == MONITOR_MAX);

	return types[p_monitor];
}

void Performance::set_process_time(double p_pt) {
	_process_time = p_pt;
}

void Performance::set_physics_process_time(double p_pt) {
	_physics_process_time = p_pt;
}

void Performance::set_navigation_process_time(double p_pt) {
	_navigation_process_time = p_pt;
}

void Performance::set_process_main_loop_time(double p_pt) {
	_process_main_loop_time = p_pt;
}

void Performance::set_process_message_queue_time(double p_pt) {
	_process_message_queue_time = p_pt;
}

void Performance::set_process_navigation_idle_time(double p_pt) {
	_process_navigation_idle_time = p_pt;
}

void Performance::set_process_render_sync_time(double p_pt) {
	_process_render_sync_time = p_pt;
}

void Performance::set_process_render_draw_time(double p_pt) {
	_process_render_draw_time = p_pt;
}

// DUMBAI: SceneTree instrumentation updates these per frame so benchmark CSV captures direct _process phase costs.
void Performance::set_process_scene_tree_fti_time(double p_pt) {
	_process_scene_tree_fti_time = p_pt;
}

void Performance::set_process_scene_tree_main_loop_time(double p_pt) {
	_process_scene_tree_main_loop_time = p_pt;
}

void Performance::set_process_scene_tree_multiplayer_time(double p_pt) {
	_process_scene_tree_multiplayer_time = p_pt;
}

void Performance::set_process_scene_tree_signal_time(double p_pt) {
	_process_scene_tree_signal_time = p_pt;
}

void Performance::set_process_scene_tree_message_queue_time(double p_pt) {
	_process_scene_tree_message_queue_time = p_pt;
}

void Performance::set_process_scene_tree_transform_flush_time(double p_pt) {
	_process_scene_tree_transform_flush_time = p_pt;
}

void Performance::set_process_scene_tree_groups_time(double p_pt) {
	_process_scene_tree_groups_time = p_pt;
}

void Performance::set_process_scene_tree_ugc_time(double p_pt) {
	_process_scene_tree_ugc_time = p_pt;
}

void Performance::set_process_scene_tree_scene_change_time(double p_pt) {
	_process_scene_tree_scene_change_time = p_pt;
}

void Performance::set_process_scene_tree_timers_time(double p_pt) {
	_process_scene_tree_timers_time = p_pt;
}

void Performance::set_process_scene_tree_tweens_time(double p_pt) {
	_process_scene_tree_tweens_time = p_pt;
}

void Performance::set_process_scene_tree_delete_queue_time(double p_pt) {
	_process_scene_tree_delete_queue_time = p_pt;
}

void Performance::set_process_scene_tree_accessibility_time(double p_pt) {
	_process_scene_tree_accessibility_time = p_pt;
}

void Performance::set_process_scene_tree_idle_callbacks_time(double p_pt) {
	_process_scene_tree_idle_callbacks_time = p_pt;
}

void Performance::set_process_scene_tree_other_time(double p_pt) {
	_process_scene_tree_other_time = p_pt;
}

void Performance::add_custom_monitor(const StringName &p_id, const Callable &p_callable, const Vector<Variant> &p_args, MonitorType p_type) {
	ERR_FAIL_COND_MSG(has_custom_monitor(p_id), "Custom monitor with id '" + String(p_id) + "' already exists.");
	_monitor_map.insert(p_id, MonitorCall(p_type, p_callable, p_args));
	_monitor_modification_time = OS::get_singleton()->get_ticks_usec();
}

void Performance::remove_custom_monitor(const StringName &p_id) {
	ERR_FAIL_COND_MSG(!has_custom_monitor(p_id), "Custom monitor with id '" + String(p_id) + "' doesn't exist.");
	_monitor_map.erase(p_id);
	_monitor_modification_time = OS::get_singleton()->get_ticks_usec();
}

bool Performance::has_custom_monitor(const StringName &p_id) {
	return _monitor_map.has(p_id);
}

Variant Performance::get_custom_monitor(const StringName &p_id) {
	ERR_FAIL_COND_V_MSG(!has_custom_monitor(p_id), Variant(), "Custom monitor with id '" + String(p_id) + "' doesn't exist.");
	bool error;
	String error_message;
	Variant return_value = _monitor_map[p_id].call(error, error_message);
	ERR_FAIL_COND_V_MSG(error, return_value, "Error calling from custom monitor '" + String(p_id) + "' to callable: " + error_message);
	return return_value;
}

TypedArray<StringName> Performance::get_custom_monitor_names() {
	if (!_monitor_map.size()) {
		return TypedArray<StringName>();
	}
	TypedArray<StringName> return_array;
	return_array.resize(_monitor_map.size());
	int index = 0;
	for (KeyValue<StringName, MonitorCall> i : _monitor_map) {
		return_array.set(index, i.key);
		index++;
	}
	return return_array;
}

Vector<int> Performance::get_custom_monitor_types() {
	if (_monitor_map.is_empty()) {
		return Vector<int>();
	}
	Vector<int> ret;
	ret.resize(_monitor_map.size());
	int index = 0;
	for (const KeyValue<StringName, MonitorCall> &i : _monitor_map) {
		ret.set(index, (int)i.value.get_monitor_type());
		index++;
	}
	return ret;
}

uint64_t Performance::get_monitor_modification_time() {
	return _monitor_modification_time;
}

Performance::Performance() {
	_process_time = 0;
	_physics_process_time = 0;
	_navigation_process_time = 0;
	_process_main_loop_time = 0;
	_process_message_queue_time = 0;
	_process_navigation_idle_time = 0;
	_process_render_sync_time = 0;
	_process_render_draw_time = 0;
	// DUMBAI: Initialize SceneTree sub-stage timings so monitors stay deterministic when instrumentation has not run yet.
	_process_scene_tree_fti_time = 0;
	_process_scene_tree_main_loop_time = 0;
	_process_scene_tree_multiplayer_time = 0;
	_process_scene_tree_signal_time = 0;
	_process_scene_tree_message_queue_time = 0;
	_process_scene_tree_transform_flush_time = 0;
	_process_scene_tree_groups_time = 0;
	_process_scene_tree_ugc_time = 0;
	_process_scene_tree_scene_change_time = 0;
	_process_scene_tree_timers_time = 0;
	_process_scene_tree_tweens_time = 0;
	_process_scene_tree_delete_queue_time = 0;
	_process_scene_tree_accessibility_time = 0;
	_process_scene_tree_idle_callbacks_time = 0;
	_process_scene_tree_other_time = 0;
	// DUMBAI: Reset render profile cache so first monitor query after startup recomputes from the latest frame profile.
	_render_profile_capture_enabled = false;
	_render_profile_cached_frame = 0;
	_render_profile_cache_valid = false;
	_render_profile_cpu_total_time = 0.0;
	_render_profile_gpu_total_time = 0.0;
	_render_profile_cpu_max_stage_time = 0.0;
	_render_profile_gpu_max_stage_time = 0.0;
	_render_profile_stage_count = 0.0;
	_render_profile_gpu_nonzero_stage_count = 0.0;
	_render_profile_gpu_zero_stage_count = 0.0;
	_monitor_modification_time = 0;
	singleton = this;
}

Performance::MonitorCall::MonitorCall(Performance::MonitorType p_type, const Callable &p_callable, const Vector<Variant> &p_arguments) {
	_type = p_type;
	_callable = p_callable;
	_arguments = p_arguments;
}

Performance::MonitorCall::MonitorCall() {
}

Variant Performance::MonitorCall::call(bool &r_error, String &r_error_message) {
	Vector<const Variant *> arguments_mem;
	arguments_mem.resize(_arguments.size());
	for (int i = 0; i < _arguments.size(); i++) {
		arguments_mem.write[i] = &_arguments[i];
	}
	const Variant **args = (const Variant **)arguments_mem.ptr();
	int argc = _arguments.size();
	Variant return_value;
	Callable::CallError error;
	_callable.callp(args, argc, return_value, error);
	r_error = (error.error != Callable::CallError::CALL_OK);
	if (r_error) {
		r_error_message = Variant::get_callable_error_text(_callable, args, argc, error);
	}
	return return_value;
}
