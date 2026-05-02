<!-- DUMBAI: This plan is scoped to eliminate benchmark harness overhead first, then attack real engine CPU/GPU bottlenecks with measurable gates. -->
# Godot Inefficiency Attack Plan

<!-- DUMBAI: Freeze objective metrics so changes are judged by reproducible benchmark outcomes, not ad-hoc impressions. -->
## Success Criteria
1. `avg_fps >= 60` in heavy phases (`warm_shadowed`, `warm_unshadowed`, `warm_shadowed_occlusion_on`).
2. `p95_frame_ms <= 16.67`.
3. `hitch_frames (>=25ms) < 1%` of sampled frames.
4. `time/render_profile_gpu_nonzero_stage_count > 0` for Vulkan runs on macOS.
5. Same benchmark config passes on 3 consecutive runs.

<!-- DUMBAI: Baseline facts keep optimization effort focused on true constraints already observed in measured runs. -->
## Current Baseline (Latest Vulkan Run)
1. Heavy phases sit around `43-49 FPS`.
2. Visible geometry is extreme (`~70M-128M` visible tris depending on phase).
3. Draw calls are high (`~1.1k-4.6k` depending on phase).
4. GPU timings export correctly on Vulkan (`device_api_name=Vulkan`, nonzero GPU stage counts).
5. CPU benchmark script work contributes large `SceneTree groups` cost during heavy phases.

<!-- DUMBAI: First remove benchmark self-cost from sampled frames so engine work is not masked by harness logic. -->
## Phase 1: Remove Harness Self-Interference
1. Add a runtime toggle to disable expensive benchmark-validation loops during timed sample frames.
2. Move `_track_visibility_swaps()` from every `_process()` tick to phase-end sampling windows.
3. Throttle or phase-end aggregate `_update_destruction_lifetimes()` bookkeeping work where possible.
4. Add per-block script timers exported to CSV:
   - `bench/time/update_camera_path_ms`
   - `bench/time/visibility_swaps_ms`
   - `bench/time/destruction_lifetimes_ms`
   - `bench/time/emit_phase_metrics_ms`
5. Accept Phase 1 only if heavy-phase `time/process_scene_tree_groups` drops by at least 30%.

<!-- DUMBAI: After harness cleanup, isolate engine CPU hotspots with explicit instrumentation in the process path. -->
## Phase 2: Engine CPU Hotspot Isolation
1. Instrument SceneTree/process internals around group processing and callback dispatch.
2. Add timing markers for:
   - group iteration overhead
   - callable dispatch cost
   - transform flush
   - delete queue processing
3. Export new performance monitors or structured logs so bottlenecks map to concrete subsystems.
4. Rank top 3 CPU hotspots by average ms and p95 ms in heavy phases.
5. Accept Phase 2 only if top hotspot attribution is stable across 3 runs.

<!-- DUMBAI: Use Vulkan GPU stage data to identify real render bottlenecks once CPU noise is reduced. -->
## Phase 3: Engine GPU Hotspot Isolation
1. Keep Vulkan as mandatory profiling backend on macOS for GPU stage visibility.
2. At phase end, print top GPU stages by total ms and max stage ms.
3. Correlate GPU stages with scene conditions:
   - shadowed vs unshadowed
   - occlusion on vs off
   - warm vs cold
4. Gate output consistency:
   - no timestamp warnings in run log
   - `gpu_nonzero_stage_count` equals `stage_count` for sampled frames.

<!-- DUMBAI: Implementation order enforces highest-yield fixes first and prevents mixing unrelated regressions. -->
## Phase 4: Optimization Order
1. Fix highest CPU hotspot first (from Phase 2).
2. Re-benchmark and require measurable CPU win before next change.
3. Fix highest GPU stage hotspot second (from Phase 3).
4. Keep absurd world triangle count, but cap visible/shadowed triangles with:
   - stricter visibility ranges
   - shadow proxy usage
   - HLOD swap distance tuning
5. Re-run all benchmark phases after each optimization patch.

<!-- DUMBAI: Final validation ensures ship decisions are made on target platform while preserving macOS regressions visibility. -->
## Phase 5: Platform Validation and Lockdown
1. Run the same benchmark suite on Windows Vulkan and treat those numbers as ship-decision baseline.
2. Re-run on macOS Vulkan and diff against Windows for regression detection.
3. Freeze benchmark scene, camera path, asset counts, and settings in-repo.
4. Block merges that fail Success Criteria.

<!-- DUMBAI: Execution checklist defines immediate next actions without ambiguity. -->
## Immediate Next Actions
1. Implement Phase 1 harness-cost timing exports and reduced per-frame validation loops.
2. Rebuild and rerun Vulkan benchmark with `--log-file bench/run.log`.
3. Compare:
   - `time/process_scene_tree_groups`
   - `time/process_main_loop`
   - heavy-phase FPS and hitch count
4. If Phase 1 gate passes, proceed to Phase 2 engine CPU instrumentation.
