/**************************************************************************/
/*  performance.h                                                         */
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

#pragma once

#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"
#include "core/variant/type_info.h"

#define PERF_WARN_OFFLINE_FUNCTION
#define PERF_WARN_PROCESS_SYNC

template <typename T>
class TypedArray;

class Performance : public Object {
	GDCLASS(Performance, Object);

	static Performance *singleton;
	static void _bind_methods();

#ifndef DISABLE_DEPRECATED
	void _add_custom_monitor_bind_compat_110433(const StringName &p_id, const Callable &p_callable, const Vector<Variant> &p_args);
	static void _bind_compatibility_methods();
#endif

	int _get_node_count() const;
	int _get_orphan_node_count() const;

	double _process_time;
	double _physics_process_time;
	double _navigation_process_time;
	// DUMBAI: Split idle-frame process cost into explicit buckets so benchmark CSV can reveal real process bottleneck.
	double _process_main_loop_time;
	double _process_message_queue_time;
	double _process_navigation_idle_time;
	double _process_render_sync_time;
	double _process_render_draw_time;
	// DUMBAI: Track SceneTree::process sub-stage timings per frame so _process bottlenecks are attributable beyond the monolithic MainLoop bucket.
	double _process_scene_tree_fti_time;
	double _process_scene_tree_main_loop_time;
	double _process_scene_tree_multiplayer_time;
	double _process_scene_tree_signal_time;
	double _process_scene_tree_message_queue_time;
	double _process_scene_tree_transform_flush_time;
	double _process_scene_tree_groups_time;
	double _process_scene_tree_ugc_time;
	double _process_scene_tree_scene_change_time;
	double _process_scene_tree_timers_time;
	double _process_scene_tree_tweens_time;
	double _process_scene_tree_delete_queue_time;
	double _process_scene_tree_accessibility_time;
	double _process_scene_tree_idle_callbacks_time;
	double _process_scene_tree_other_time;
	// DUMBAI: Cache render-profile derived timings so multiple monitor queries in one frame do not rescan all GPU timestamp areas.
	mutable bool _render_profile_capture_enabled = false;
	mutable uint64_t _render_profile_cached_frame = 0;
	mutable bool _render_profile_cache_valid = false;
	mutable double _render_profile_cpu_total_time = 0.0;
	mutable double _render_profile_gpu_total_time = 0.0;
	mutable double _render_profile_cpu_max_stage_time = 0.0;
	mutable double _render_profile_gpu_max_stage_time = 0.0;
	// DUMBAI: Track frame-profile sample health so benchmark CSV can prove whether GPU timestamps are present or all flat zero.
	mutable double _render_profile_stage_count = 0.0;
	mutable double _render_profile_gpu_nonzero_stage_count = 0.0;
	mutable double _render_profile_gpu_zero_stage_count = 0.0;

public:
	enum Monitor {
		TIME_FPS,
		TIME_PROCESS,
		TIME_PHYSICS_PROCESS,
		TIME_NAVIGATION_PROCESS,
		MEMORY_STATIC,
		MEMORY_STATIC_MAX,
		MEMORY_MESSAGE_BUFFER_MAX,
		OBJECT_COUNT,
		OBJECT_RESOURCE_COUNT,
		OBJECT_NODE_COUNT,
		OBJECT_ORPHAN_NODE_COUNT,
		RENDER_TOTAL_OBJECTS_IN_FRAME,
		RENDER_TOTAL_PRIMITIVES_IN_FRAME,
		RENDER_TOTAL_DRAW_CALLS_IN_FRAME,
		RENDER_VIDEO_MEM_USED,
		RENDER_TEXTURE_MEM_USED,
		RENDER_BUFFER_MEM_USED,
		PHYSICS_2D_ACTIVE_OBJECTS,
		PHYSICS_2D_COLLISION_PAIRS,
		PHYSICS_2D_ISLAND_COUNT,
		PHYSICS_3D_ACTIVE_OBJECTS,
		PHYSICS_3D_COLLISION_PAIRS,
		PHYSICS_3D_ISLAND_COUNT,
		AUDIO_OUTPUT_LATENCY,
		// Deprecated, use the 2D/3D specific ones instead.
		NAVIGATION_ACTIVE_MAPS,
		NAVIGATION_REGION_COUNT,
		NAVIGATION_AGENT_COUNT,
		NAVIGATION_LINK_COUNT,
		NAVIGATION_POLYGON_COUNT,
		NAVIGATION_EDGE_COUNT,
		NAVIGATION_EDGE_MERGE_COUNT,
		NAVIGATION_EDGE_CONNECTION_COUNT,
		NAVIGATION_EDGE_FREE_COUNT,
		NAVIGATION_OBSTACLE_COUNT,
		PIPELINE_COMPILATIONS_CANVAS,
		PIPELINE_COMPILATIONS_MESH,
		PIPELINE_COMPILATIONS_SURFACE,
		PIPELINE_COMPILATIONS_DRAW,
		PIPELINE_COMPILATIONS_SPECIALIZATION,
		NAVIGATION_2D_ACTIVE_MAPS,
		NAVIGATION_2D_REGION_COUNT,
		NAVIGATION_2D_AGENT_COUNT,
		NAVIGATION_2D_LINK_COUNT,
		NAVIGATION_2D_POLYGON_COUNT,
		NAVIGATION_2D_EDGE_COUNT,
		NAVIGATION_2D_EDGE_MERGE_COUNT,
		NAVIGATION_2D_EDGE_CONNECTION_COUNT,
		NAVIGATION_2D_EDGE_FREE_COUNT,
		NAVIGATION_2D_OBSTACLE_COUNT,
	#ifndef _3D_DISABLED
			NAVIGATION_3D_ACTIVE_MAPS,
			NAVIGATION_3D_REGION_COUNT,
			NAVIGATION_3D_AGENT_COUNT,
			NAVIGATION_3D_LINK_COUNT,
			NAVIGATION_3D_POLYGON_COUNT,
			NAVIGATION_3D_EDGE_COUNT,
			NAVIGATION_3D_EDGE_MERGE_COUNT,
			NAVIGATION_3D_EDGE_CONNECTION_COUNT,
			NAVIGATION_3D_EDGE_FREE_COUNT,
			NAVIGATION_3D_OBSTACLE_COUNT,
	#endif // _3D_DISABLED
			TIME_PROCESS_MAIN_LOOP,
			TIME_PROCESS_MESSAGE_QUEUE,
			TIME_PROCESS_NAVIGATION_IDLE,
			TIME_PROCESS_RENDER_SYNC,
			TIME_PROCESS_RENDER_DRAW,
			// DUMBAI: SceneTree idle-phase sub-stage monitors provide direct visibility into where MainLoop::process() time is spent.
			TIME_PROCESS_SCENE_TREE_FTI,
			TIME_PROCESS_SCENE_TREE_MAIN_LOOP,
			TIME_PROCESS_SCENE_TREE_MULTIPLAYER,
			TIME_PROCESS_SCENE_TREE_SIGNAL,
			TIME_PROCESS_SCENE_TREE_MESSAGE_QUEUE,
			TIME_PROCESS_SCENE_TREE_TRANSFORM_FLUSH,
			TIME_PROCESS_SCENE_TREE_GROUPS,
			TIME_PROCESS_SCENE_TREE_UGC,
			TIME_PROCESS_SCENE_TREE_SCENE_CHANGE,
			TIME_PROCESS_SCENE_TREE_TIMERS,
			TIME_PROCESS_SCENE_TREE_TWEENS,
			TIME_PROCESS_SCENE_TREE_DELETE_QUEUE,
			TIME_PROCESS_SCENE_TREE_ACCESSIBILITY,
			TIME_PROCESS_SCENE_TREE_IDLE_CALLBACKS,
			TIME_PROCESS_SCENE_TREE_OTHER,
			// DUMBAI: Render-stage monitors expose renderer CPU/GPU timing needed to isolate spikes against a 16.67ms budget.
			TIME_RENDER_FRAME_SETUP_CPU,
			TIME_RENDER_PROFILE_CPU_TOTAL,
			TIME_RENDER_PROFILE_GPU_TOTAL,
			TIME_RENDER_PROFILE_CPU_MAX_STAGE,
			TIME_RENDER_PROFILE_GPU_MAX_STAGE,
			TIME_RENDER_PROFILE_STAGE_COUNT,
			TIME_RENDER_PROFILE_GPU_NONZERO_STAGE_COUNT,
			TIME_RENDER_PROFILE_GPU_ZERO_STAGE_COUNT,
			MONITOR_MAX
		};

	enum MonitorType {
		MONITOR_TYPE_QUANTITY,
		MONITOR_TYPE_MEMORY,
		MONITOR_TYPE_TIME,
		MONITOR_TYPE_PERCENTAGE,
	};

	double get_monitor(Monitor p_monitor) const;
	String get_monitor_name(Monitor p_monitor) const;

	MonitorType get_monitor_type(Monitor p_monitor) const;

	void set_process_time(double p_pt);
	void set_physics_process_time(double p_pt);
	void set_navigation_process_time(double p_pt);
	void set_process_main_loop_time(double p_pt);
	void set_process_message_queue_time(double p_pt);
	void set_process_navigation_idle_time(double p_pt);
	void set_process_render_sync_time(double p_pt);
	void set_process_render_draw_time(double p_pt);
	void set_process_scene_tree_fti_time(double p_pt);
	void set_process_scene_tree_main_loop_time(double p_pt);
	void set_process_scene_tree_multiplayer_time(double p_pt);
	void set_process_scene_tree_signal_time(double p_pt);
	void set_process_scene_tree_message_queue_time(double p_pt);
	void set_process_scene_tree_transform_flush_time(double p_pt);
	void set_process_scene_tree_groups_time(double p_pt);
	void set_process_scene_tree_ugc_time(double p_pt);
	void set_process_scene_tree_scene_change_time(double p_pt);
	void set_process_scene_tree_timers_time(double p_pt);
	void set_process_scene_tree_tweens_time(double p_pt);
	void set_process_scene_tree_delete_queue_time(double p_pt);
	void set_process_scene_tree_accessibility_time(double p_pt);
	void set_process_scene_tree_idle_callbacks_time(double p_pt);
	void set_process_scene_tree_other_time(double p_pt);

	void add_custom_monitor(const StringName &p_id, const Callable &p_callable, const Vector<Variant> &p_args, MonitorType p_type = MONITOR_TYPE_QUANTITY);
	void remove_custom_monitor(const StringName &p_id);
	bool has_custom_monitor(const StringName &p_id);
	Variant get_custom_monitor(const StringName &p_id);
	TypedArray<StringName> get_custom_monitor_names();
	Vector<int> get_custom_monitor_types();
	Array get_render_profile_stage_breakdown(int p_limit = 8, bool p_sort_by_gpu_total = true) const;
	// DUMBAI: Expose forward-clustered pass workload counters so benchmark logs can connect GPU timings to concrete render list pressure.
	Dictionary get_render_forward_clustered_workload_snapshot() const;

	uint64_t get_monitor_modification_time();

	static Performance *get_singleton() { return singleton; }

	Performance();

private:
	class MonitorCall {
		MonitorType _type = MONITOR_TYPE_QUANTITY;
		Callable _callable;
		Vector<Variant> _arguments;

	public:
		MonitorCall(MonitorType p_type, const Callable &p_callable, const Vector<Variant> &p_arguments);
		MonitorCall();
		Variant call(bool &r_error, String &r_error_message);
		inline MonitorType get_monitor_type() const { return _type; }
	};

	HashMap<StringName, MonitorCall> _monitor_map;
	uint64_t _monitor_modification_time;

	void _ensure_render_profile_capture_enabled() const;
	void _refresh_render_profile_cache() const;
};

VARIANT_ENUM_CAST(Performance::Monitor);
VARIANT_ENUM_CAST(Performance::MonitorType);
