# GPU-Driven Rendering Migration Plan (RD Renderers)

## Objective
Move Godot's RD renderers from CPU-driven scene culling + CPU-built render lists to GPU-driven visibility + GPU-built indirect draws, while preserving correctness, debuggability, and fallback behavior.

## Scope
1. In scope:
   - Forward+ (`RenderForwardClustered`) and Mobile (`RenderForwardMobile`) pipelines.
   - CPU culling/list build replacement for depth/opaque/shadow first, then transparent.
   - GPU occlusion for RD renderers.
2. Out of scope:
   - Compatibility (`gl_compatibility`) renderer path.
   - Full removal of CPU path in early phases.
   - Material/shader feature redesign.

## Current Architecture Snapshot (Source Anchors)
1. CPU scene cull is done in `RendererSceneCull::_scene_cull()`:
   - `servers/rendering/renderer_scene_cull.cpp`
2. CPU cull applies frustum/layer/visibility range/occlusion checks:
   - `IN_FRUSTUM`, `VIS_CHECK`, `OCCLUSION_CULLED` checks in `servers/rendering/renderer_scene_cull.cpp`
3. CPU cull outputs paged arrays in `InstanceCullResult`:
   - `servers/rendering/renderer_scene_cull.h`
4. Scene render is fed CPU-built arrays via `scene_render->render_scene(...)`:
   - `servers/rendering/renderer_scene_cull.cpp`
5. CPU occlusion buffer update currently calls `RendererSceneOcclusionCull::buffer_update(...)`:
   - `servers/rendering/renderer_scene_cull.cpp`
6. Current occlusion implementation is CPU raycast/Embree:
   - `modules/raycast/raycast_occlusion_cull.cpp`
7. Forward+/Mobile build render lists on CPU:
   - `_fill_render_list()` in
     - `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp`
     - `servers/rendering/renderer_rd/forward_mobile/render_forward_mobile.cpp`
8. Draw submission is mostly CPU-iterated draw calls:
   - `_render_list_template()` in
     - `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp`
     - `servers/rendering/renderer_rd/forward_mobile/render_forward_mobile.cpp`
9. RD already supports indirect draw + compute dispatch indirect:
   - `draw_list_draw_indirect()` and `compute_list_dispatch_indirect()` in `servers/rendering/rendering_device.h`
10. GPU cluster build exists and can be reused:
   - `ClusterBuilderRD::bake_cluster()` in `servers/rendering/renderer_rd/cluster_builder_rd.cpp`

## Target Architecture (End State)
1. CPU updates scene state buffers only (transforms, bounds, instance flags, material/surface metadata).
2. GPU compute performs:
   - Frustum/layer/LOD visibility.
   - Optional GPU occlusion test (Hi-Z based).
   - Per-pass compaction into visible instance/surface lists.
3. GPU compute writes indirect draw command buffers and instance remap buffers.
4. Graphics passes submit mostly via `draw_indirect`.
5. CPU fallback path remains selectable at runtime for debugging and unsupported devices.

## Architectural Principles
1. Incremental rollout with hard runtime switch.
2. "No full cutover" until perf and correctness gates pass across test matrix.
3. Forward+ first, Mobile second.
4. Preserve deterministic debug path (CPU list mode) for regression triage.
5. Avoid "all-at-once" replacement; migrate pass families independently.

## Phase 0: Guardrails, Feature Flags, and Telemetry
### Deliverables
1. Add runtime/project setting for GPU-driven path, default `off`.
2. Add runtime fallback reasons (unsupported feature, validation failure, debug override).
3. Add frame telemetry:
   - CPU cull ms.
   - GPU cull ms.
   - CPU list build ms.
   - indirect draw count and generated command count.
   - visible instance/surface counts per pass.

### Code Touchpoints
1. `servers/rendering/renderer_scene_cull.cpp`
2. `servers/rendering/renderer_viewport.cpp`
3. `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp`
4. `servers/rendering/renderer_rd/forward_mobile/render_forward_mobile.cpp`

### Exit Criteria
1. Toggle can force CPU path and GPU path at runtime.
2. Telemetry is emitted and stable for 300+ frames in benchmark scenes.

## Phase 1: Persistent GPU Scene Database
### Deliverables
1. New RD-side scene buffers for:
   - instance transforms and bounds.
   - per-instance flags/layer masks/LOD params.
   - per-surface draw metadata (mesh surface index, material/pipeline key, index/vertex info, base instance mapping).
2. Dirty-region update path from existing geometry update logic.
3. Triple-buffered or frame-ring strategy to avoid CPU/GPU write-read hazards.

### Suggested New Components
1. `servers/rendering/renderer_rd/gpu_scene_storage_rd.h`
2. `servers/rendering/renderer_rd/gpu_scene_storage_rd.cpp`
3. `servers/rendering/renderer_rd/shaders/gpu_scene_cull.glsl` (or split shader files by pass)

### Integration Touchpoints
1. `_update_dirty_geometry_instances()` usage in both Forward+ and Mobile:
   - `render_forward_clustered.cpp`
   - `render_forward_mobile.cpp`
2. Data currently packed into CPU instance buffers in `_fill_instance_data()` becomes source of GPU scene buffer writes.

### Exit Criteria
1. All visible draw candidates in a frame can be represented in GPU scene buffers.
2. No per-frame full buffer rebuild required except fallback/debug mode.

## Phase 2: GPU Visibility Culling (Frustum/Layer/LOD)
### Deliverables
1. Compute kernel that performs:
   - frustum check against world-space bounds.
   - layer mask filtering.
   - visibility range/LOD gate.
2. Append/compact visible IDs into GPU buffers.
3. Produce per-pass visibility lists (depth/opaque/shadow at minimum).

### Notes
1. Keep CPU cull functional in parallel for frame-by-frame parity checks.
2. Add optional validation mode comparing CPU visible set and GPU visible set with tolerance counters.

### Integration Touchpoints
1. Replace dependency on CPU `InstanceCullResult::geometry_instances` for migrated passes.
2. Keep CPU lists for unmigrated passes and for fallback.

### Exit Criteria
1. Visibility parity reaches agreed threshold in representative scenes:
   - Example gate: >= 99.5% matching IDs over 1000 sampled frames.
2. No major visual popping/regression in camera stress tests.

## Phase 3: GPU Occlusion for RD Renderers
### Deliverables
1. Add depth pyramid (Hi-Z) generation pass from prepass depth.
2. Add conservative occlusion kernel testing instance bounds against Hi-Z.
3. Integrate occlusion result into visibility compaction pipeline.

### Fallback Strategy
1. Keep current CPU Embree occlusion for:
   - debug parity checks.
   - forced fallback mode.
   - non-RD paths.
2. Add runtime option: `cpu_occlusion`, `gpu_occlusion`, `off`.

### Integration Touchpoints
1. `servers/rendering/renderer_scene_cull.cpp` occlusion update call sites.
2. `servers/rendering/renderer_viewport.cpp` occlusion feature controls.
3. Existing CPU occlusion implementation remains in:
   - `modules/raycast/raycast_occlusion_cull.cpp`

### Exit Criteria
1. Occlusion correctness validated in indoor and outdoor benchmark scenes.
2. GPU occlusion reduces CPU frame cost without introducing objectionable false negatives.

## Phase 4: GPU Draw Command Generation (Indirect)
### Deliverables
1. Compute pass that builds indirect command buffers:
   - command structure per surface/material bucket.
   - draw count buffers.
   - instance remap tables.
2. Transition migrated passes to `draw_list_draw_indirect(...)`.
3. Maintain per-pass command buffer lifecycle and synchronization.

### Integration Touchpoints
1. Forward+ draw loop:
   - `_render_list_template()` in `render_forward_clustered.cpp`
2. Mobile draw loop:
   - `_render_list_template()` in `render_forward_mobile.cpp`
3. RD interfaces:
   - `servers/rendering/rendering_device.h`

### Exit Criteria
1. Depth and opaque passes submit mostly via indirect draws.
2. CPU draw-loop overhead drops measurably in heavy scenes.

## Phase 5: Forward+ Pass Family Migration
### Order
1. Depth prepass.
2. Opaque color pass.
3. Shadow passes.
4. Motion vectors.
5. Transparent pass (last due to sorting constraints).

### Transparent Path Strategy
1. Start hybrid:
   - GPU visibility and candidate compaction.
   - CPU-assisted sorting for alpha where strict order is required.
2. Later option:
   - depth-bin + material-bin GPU sort approximation where acceptable.

### Integration Touchpoints
1. `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp`
2. Cluster-light path remains active:
   - `servers/rendering/renderer_rd/cluster_builder_rd.cpp`
3. Light/decal/reflection setup hooks used by cluster builder:
   - `servers/rendering/renderer_rd/storage_rd/light_storage.cpp`

### Exit Criteria
1. Forward+ default path can run GPU-driven for all opaque/depth/shadow workloads.
2. Benchmark scenes show stable or better frame time with no major visual regressions.

## Phase 6: Mobile Migration
### Deliverables
1. Reuse GPU scene/cull/indirect framework with Mobile-specific tuning:
   - lower memory budgets.
   - smaller workgroup sizes where needed.
2. Migrate depth/opaque first; keep transparent hybrid until validated.

### Integration Touchpoints
1. `servers/rendering/renderer_rd/forward_mobile/render_forward_mobile.cpp`

### Exit Criteria
1. Mobile backend gains CPU reduction in heavy scenes.
2. Device coverage tests pass for supported RD drivers.

## Phase 7: Validation, Rollout, and Default Switch
### Validation Matrix
1. Platforms:
   - Windows (Vulkan, D3D12 where applicable)
   - Linux (Vulkan)
   - macOS (Metal via RD)
2. Renderers:
   - Forward+
   - Mobile
3. Scene types:
   - indoor high-occlusion
   - outdoor low-occlusion
   - many small meshes
   - large multi-mesh heavy content
4. Features:
   - shadows on/off
   - MSAA variants
   - TAA/motion vectors
   - stereo/XR views

### Metrics Gates
1. Functional:
   - image diff and artifact triage pass on golden scenes.
2. Performance:
   - CPU frame-time reduction in visibility/list build stages.
   - no >5% regression in GPU-bound scenes unless justified.
3. Stability:
   - no crash/hang over long soak runs.

### Rollout
1. Default `off` in first release cycle after merge.
2. Default `on` for Forward+ only after gates pass and fallback path is proven.
3. Mobile default change deferred until device matrix confidence is high.

## Work Breakdown Structure (Implementation Granularity)
1. WBS-1: Flags + telemetry + debug visualization of visible counts.
2. WBS-2: GPU scene storage allocation/update path.
3. WBS-3: Frustum/layer/LOD cull compute shaders + dispatch orchestration.
4. WBS-4: Hi-Z generation + occlusion compute.
5. WBS-5: Indirect command generation and draw-loop integration.
6. WBS-6: Forward+ depth/opaque/shadow migration.
7. WBS-7: Forward+ transparent hybrid path.
8. WBS-8: Mobile depth/opaque migration.
9. WBS-9: Cross-platform validation harness and comparison tooling.
10. WBS-10: Default-switch and cleanup.

## Risk Register and Mitigations
1. Risk: GPU/driver divergence across Vulkan/D3D12/Metal.
   - Mitigation: keep CPU fallback + per-driver blacklist toggles.
2. Risk: Occlusion false negatives causing visible popping.
   - Mitigation: conservative test thresholds, hysteresis, fallback mode.
3. Risk: Transparent sorting regressions.
   - Mitigation: staged hybrid transparent path; keep CPU sorting as guardrail.
4. Risk: Memory pressure from persistent GPU scene buffers.
   - Mitigation: configurable budgets, pooled allocations, compact formats.
5. Risk: Sync hazards (CPU writes vs GPU reads).
   - Mitigation: frame-ring buffering + explicit barriers + validation asserts.
6. Risk: Hard-to-debug visual mismatches.
   - Mitigation: debug overlays for cull decisions and CPU/GPU set diff counters.

## Debug and Tooling Requirements
1. Runtime debug draws:
   - number of visible instances from CPU and GPU paths.
   - culled-by-frustum/occlusion counts.
2. Capture dumps:
   - optional buffer snapshots for one frame (visible IDs, indirect commands).
3. Validation mode:
   - run CPU and GPU cull in parallel and report mismatch stats.

## Dependencies
1. Reliable RD compute/indirect behavior on target drivers.
2. Existing cluster/light buffer update path remains compatible with new visible lists.
3. Benchmark scenes and reproducible perf harness for gate decisions.

## Completion Definition
1. Forward+ and Mobile can render main opaque/depth/shadow paths from GPU-generated visibility + indirect commands.
2. CPU path remains available and stable as fallback/debug mode.
3. Cross-platform validation gates pass.
4. GPU-driven mode can be enabled by default (at least for Forward+) with acceptable risk.

## Immediate Execution Order
1. Implement Phase 0 (flags + telemetry).
2. Implement Phase 1 (GPU scene storage).
3. Implement Phase 2 for depth/opaque visibility only.
4. Add Phase 4 indirect command generation for depth/opaque.
5. Integrate Forward+ migrated passes before tackling occlusion/transparent complexity.
