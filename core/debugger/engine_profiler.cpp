/**************************************************************************/
/*  engine_profiler.cpp                                                   */
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

#include "engine_profiler.h"

#include "core/config/engine.h"
#include "core/debugger/engine_debugger.h"
#include "core/object/class_db.h" // IWYU pragma: keep. `GDVIRTUAL_BIND` macro.
#include "main/performance.h"

namespace {

Dictionary _build_update_metrics(double p_frame_time, double p_process_time, double p_physics_time, double p_physics_frame_time) {
	Dictionary metrics;
	metrics["frame_time"] = p_frame_time;
	metrics["process_time"] = p_process_time;
	metrics["physics_time"] = p_physics_time;
	metrics["physics_frame_time"] = p_physics_frame_time;

	if (const Engine *engine = Engine::get_singleton()) {
		metrics["fps"] = engine->get_frames_per_second();
		metrics["frame_ticks"] = engine->get_frame_ticks();
		metrics["process_step"] = engine->get_process_step();
		metrics["process_frames"] = engine->get_process_frames();
		metrics["physics_frames"] = engine->get_physics_frames();
		metrics["physics_interpolation_fraction"] = engine->get_physics_interpolation_fraction();
		metrics["in_physics_frame"] = engine->is_in_physics_frame();
		metrics["time_scale"] = engine->get_time_scale();
		metrics["effective_time_scale"] = engine->get_effective_time_scale();
		metrics["physics_ticks_per_second"] = engine->get_physics_ticks_per_second();
		metrics["max_physics_steps_per_frame"] = engine->get_max_physics_steps_per_frame();
		metrics["max_fps"] = engine->get_max_fps();
	}

	if (const Performance *performance = Performance::get_singleton()) {
		Dictionary process_breakdown;
		process_breakdown["main_loop"] = performance->get_monitor(Performance::TIME_PROCESS_MAIN_LOOP);
		process_breakdown["message_queue"] = performance->get_monitor(Performance::TIME_PROCESS_MESSAGE_QUEUE);
		process_breakdown["navigation_idle"] = performance->get_monitor(Performance::TIME_PROCESS_NAVIGATION_IDLE);
		process_breakdown["render_sync"] = performance->get_monitor(Performance::TIME_PROCESS_RENDER_SYNC);
		process_breakdown["render_draw"] = performance->get_monitor(Performance::TIME_PROCESS_RENDER_DRAW);
		process_breakdown["scene_tree_fti"] = performance->get_monitor(Performance::TIME_PROCESS_SCENE_TREE_FTI);
		process_breakdown["scene_tree_main_loop"] = performance->get_monitor(Performance::TIME_PROCESS_SCENE_TREE_MAIN_LOOP);
		process_breakdown["scene_tree_multiplayer"] = performance->get_monitor(Performance::TIME_PROCESS_SCENE_TREE_MULTIPLAYER);
		process_breakdown["scene_tree_signal"] = performance->get_monitor(Performance::TIME_PROCESS_SCENE_TREE_SIGNAL);
		process_breakdown["scene_tree_message_queue"] = performance->get_monitor(Performance::TIME_PROCESS_SCENE_TREE_MESSAGE_QUEUE);
		process_breakdown["scene_tree_transform_flush"] = performance->get_monitor(Performance::TIME_PROCESS_SCENE_TREE_TRANSFORM_FLUSH);
		process_breakdown["scene_tree_groups"] = performance->get_monitor(Performance::TIME_PROCESS_SCENE_TREE_GROUPS);
		process_breakdown["scene_tree_ugc"] = performance->get_monitor(Performance::TIME_PROCESS_SCENE_TREE_UGC);
		process_breakdown["scene_tree_scene_change"] = performance->get_monitor(Performance::TIME_PROCESS_SCENE_TREE_SCENE_CHANGE);
		process_breakdown["scene_tree_timers"] = performance->get_monitor(Performance::TIME_PROCESS_SCENE_TREE_TIMERS);
		process_breakdown["scene_tree_tweens"] = performance->get_monitor(Performance::TIME_PROCESS_SCENE_TREE_TWEENS);
		process_breakdown["scene_tree_delete_queue"] = performance->get_monitor(Performance::TIME_PROCESS_SCENE_TREE_DELETE_QUEUE);
		process_breakdown["scene_tree_accessibility"] = performance->get_monitor(Performance::TIME_PROCESS_SCENE_TREE_ACCESSIBILITY);
		process_breakdown["scene_tree_idle_callbacks"] = performance->get_monitor(Performance::TIME_PROCESS_SCENE_TREE_IDLE_CALLBACKS);
		process_breakdown["scene_tree_other"] = performance->get_monitor(Performance::TIME_PROCESS_SCENE_TREE_OTHER);
		metrics["process_breakdown"] = process_breakdown;

		Dictionary render_profile;
		render_profile["frame_setup_cpu"] = performance->get_monitor(Performance::TIME_RENDER_FRAME_SETUP_CPU);
		render_profile["cpu_total"] = performance->get_monitor(Performance::TIME_RENDER_PROFILE_CPU_TOTAL);
		render_profile["gpu_total"] = performance->get_monitor(Performance::TIME_RENDER_PROFILE_GPU_TOTAL);
		render_profile["cpu_max_stage"] = performance->get_monitor(Performance::TIME_RENDER_PROFILE_CPU_MAX_STAGE);
		render_profile["gpu_max_stage"] = performance->get_monitor(Performance::TIME_RENDER_PROFILE_GPU_MAX_STAGE);
		render_profile["stage_count"] = performance->get_monitor(Performance::TIME_RENDER_PROFILE_STAGE_COUNT);
		render_profile["gpu_nonzero_stage_count"] = performance->get_monitor(Performance::TIME_RENDER_PROFILE_GPU_NONZERO_STAGE_COUNT);
		render_profile["gpu_zero_stage_count"] = performance->get_monitor(Performance::TIME_RENDER_PROFILE_GPU_ZERO_STAGE_COUNT);
		metrics["render_profile"] = render_profile;
	}

	return metrics;
}

} // namespace

void EngineProfiler::_bind_methods() {
	GDVIRTUAL_BIND(_toggle, "enable", "options");
	GDVIRTUAL_BIND(_add_frame, "data");
	GDVIRTUAL_BIND(_update, "metrics");
	GDVIRTUAL_BIND(_tick, "frame_time", "process_time", "physics_time", "physics_frame_time");
}

void EngineProfiler::toggle(bool p_enable, const Array &p_array) {
	GDVIRTUAL_CALL(_toggle, p_enable, p_array);
}

void EngineProfiler::add(const Array &p_data) {
	GDVIRTUAL_CALL(_add_frame, p_data);
}

void EngineProfiler::update(const Dictionary &p_data) {
	GDVIRTUAL_CALL(_update, p_data);
}

void EngineProfiler::tick(double p_frame_time, double p_process_time, double p_physics_time, double p_physics_frame_time) {
	GDVIRTUAL_CALL(_tick, p_frame_time, p_process_time, p_physics_time, p_physics_frame_time);
	if (GDVIRTUAL_IS_OVERRIDDEN(_update)) {
		update(_build_update_metrics(p_frame_time, p_process_time, p_physics_time, p_physics_frame_time));
	}
}

Error EngineProfiler::bind(const String &p_name) {
	ERR_FAIL_COND_V(is_bound(), ERR_ALREADY_IN_USE);
	EngineDebugger::Profiler prof(
			this,
			[](void *p_user, bool p_enable, const Array &p_opts) {
				static_cast<EngineProfiler *>(p_user)->toggle(p_enable, p_opts);
			},
			[](void *p_user, const Array &p_data) {
				static_cast<EngineProfiler *>(p_user)->add(p_data);
			},
			[](void *p_user, double p_frame_time, double p_process_time, double p_physics_time, double p_physics_frame_time) {
				static_cast<EngineProfiler *>(p_user)->tick(p_frame_time, p_process_time, p_physics_time, p_physics_frame_time);
			});
	registration = p_name;
	EngineDebugger::register_profiler(p_name, prof);
	return OK;
}

Error EngineProfiler::unbind() {
	ERR_FAIL_COND_V(!is_bound(), ERR_UNCONFIGURED);
	EngineDebugger::unregister_profiler(registration);
	registration.clear();
	return OK;
}

EngineProfiler::~EngineProfiler() {
	if (is_bound()) {
		unbind();
	}
}
