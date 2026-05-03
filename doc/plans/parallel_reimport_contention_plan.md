# Parallel (Re)Import Contention-Avoidance Plan

## Context

Large asset sets in `res://unreal_exports/...` show long (re)import times and low CPU utilization during scene-heavy phases.

Current behavior in this branch:

- Thread-safe importers (for example `texture`) can run in parallel.
- Scene-family importers are currently forced to serial in `EditorFileSystem` to avoid stalls/deadlocks.
- Per-asset CSV timing is already available at:
  - `res://.godot/editor/import_asset_timings.csv`

Recent telemetry indicates:

- Parallel texture batch execution is working (`threaded=1`, `threads_used>1`).
- Scene imports are serial (`threaded=0`, `threads_used=1`).
- Previous stall signatures occurred during aggressive threaded batches, with `asset_start` events and no matching `asset` completion events.

## Problem Statement

We need to safely parallelize imports (including scene-heavy projects) without reintroducing deadlocks/stalls or corrupting editor/shared state.

## Root Cause Hypothesis

`EditorFileSystem::_reimport_file()` currently mixes:

1. Worker-safe heavy import work (importer execution, file generation, `.import`/`.md5` writes).
2. Shared editor/global state mutation (filesystem cache nodes, `ResourceUID`, `ResourceCache`, preview invalidation, signals).

When these responsibilities run on worker threads, contention risk is high.  
Scene importer path additionally touches editor-facing systems (preview generation and post-import hooks), which is not a good fit for unrestricted worker execution.

## Goals

1. Keep import throughput high with bounded parallelism.
2. Avoid deadlocks and long liveness stalls.
3. Preserve correctness of `.import` metadata, UID mapping, cache reloads, and editor updates.
4. Make contention observable with clear timing and phase telemetry.

## Non-Goals

1. Changing resource import format semantics.
2. Rewriting all importer implementations at once.
3. Removing existing serial fallback behavior.

## Design Principles

1. **Two-phase pipeline**: worker phase (pure import) + commit phase (shared/editor state).
2. **Main-thread ownership** for editor-facing side effects.
3. **Importer-specific concurrency caps** (no one-size-fits-all worker count).
4. **Adaptive fallback** on liveness risk.
5. **Always-observable** execution with per-phase timings.

## Proposed Architecture

### A) Worker/Commit Split in `EditorFileSystem`

Introduce a result payload for import work:

- Suggested struct (in `editor/file_system/editor_file_system.h`):
  - `path`, `importer_name`, `uid`
  - `error`
  - `dest_paths`, `gen_files`, `deps`
  - `resource_type`, `resource_script_class`, `import_valid`
  - `source_mtime`, `import_mtime`, `import_md5`
  - `timing_ms` and batch metadata

Extract `_reimport_file()` into:

1. `*_worker(...)`:
  - Resolve importer/options
  - Run `importer->import(...)`
  - Write `.import` and `.md5`
  - Return `ImportWorkResult`
  - No `fs->files` mutation
  - No `ResourceUID` global mutation
  - No `EditorResourcePreview` calls
  - No signal emission

2. `*_commit(const ImportWorkResult &)` (main thread only):
  - Update `EditorFileSystemDirectory::FileInfo`
  - Update `ResourceUID` map
  - Reload cached generated resources if needed
  - Invalidate preview/cache
  - Queue UI/editor notifications

### B) Threaded Reimport Result Queue

In threaded mode:

1. Workers execute only `*_worker(...)`.
2. Workers push `ImportWorkResult` into a mutex-protected queue.
3. Main thread waits/polls completion and drains queue, running `*_commit(...)`.

This keeps CPU-heavy work parallel while centralizing shared-state writes.

### C) Scene Importer Safe Parallel Path

Scene import path should support a worker-safe mode:

1. Worker-safe branch for parse/convert/save.
2. Defer editor-facing operations to commit/main-thread phase:
  - scene preview generation
  - editor error UI hooks
  - post-import script/plugin operations that require main-thread/editor state

Initial conservative rule:

- Allow scene parallelism with low cap (`2` workers) only when worker-safe mode is active.
- Keep automatic serial fallback.

## Detailed Implementation Phases

## Phase 0: Stabilize Baseline (already in progress)

1. Keep timing CSV with `asset_start` and `asset` phases.
2. Keep batch metadata (`threads_used`, `batch_size`, `thread_id`).
3. Keep liveness heartbeat in reimport wait loop.

Exit criteria:

- No opaque freezes: every stall has phase data.

## Phase 1: Introduce Import Work Result + Worker/Commit Split

Files:

- `editor/file_system/editor_file_system.h`
- `editor/file_system/editor_file_system.cpp`

Tasks:

1. Add `ImportWorkResult` and queue containers.
2. Extract worker-safe logic from `_reimport_file(...)`.
3. Implement main-thread commit function.
4. Convert serial path to use worker+commit as well (single execution path).

Exit criteria:

- Functional parity with current serial behavior.
- All tests/import flows pass with threading disabled.

## Phase 2: Threaded Queue Commit Path

Files:

- `editor/file_system/editor_file_system.cpp`

Tasks:

1. Change `_reimport_thread(...)` to worker-only import.
2. Push results to queue and post semaphore.
3. Main thread drains queue and commits per result.
4. Preserve importer group lifecycle (`import_threaded_begin/end`).

Exit criteria:

- Threaded texture imports complete without stale `asset_start` entries.
- No editor filesystem cache inconsistencies after import.

## Phase 3: Scene Parallel Safety

Files:

- `editor/import/3d/resource_importer_scene.cpp`
- `editor/import/3d/resource_importer_scene.h`
- `editor/file_system/editor_file_system.cpp`

Tasks:

1. Add worker-safe scene import mode.
2. Defer preview generation to commit phase.
3. Gate post-import script/plugin execution to safe context.
4. Re-enable scene threaded scheduling behind a feature flag with low cap.

Exit criteria:

- Scene batches run with `threaded=1` and `threads_used>1` without stalls.
- Output parity checks pass on representative scene sets.

## Phase 4: Adaptive Scheduler and Contention Controls

Files:

- `editor/file_system/editor_file_system.cpp`
- `editor/settings/editor_settings.cpp`

Tasks:

1. Add per-importer worker caps, default:
  - `scene`: 2
  - `texture`: 8-12 (hardware dependent)
  - fallback default: `pool-1`
2. Add liveness watchdog policy:
  - if no completion progress for timeout window, reduce concurrency or force serial for that importer for remaining items.
3. Add project/editor settings for tuning.

Exit criteria:

- No deadlocks under stress (multiple repeated full reimport cycles).
- CPU utilization increases in scene-heavy windows compared to baseline.

## Phase 5: Telemetry and Validation

Files:

- `editor/file_system/editor_file_system.cpp`

Tasks:

1. Extend CSV phases:
  - `worker_start`, `worker_done`, `commit_start`, `commit_done`
2. Add fallback reason/event rows.
3. Add batch wall-time summary rows.

Exit criteria:

- Every import can be reconstructed phase-by-phase from CSV.

## Testing Strategy

1. Functional:
  - Full reimport on representative `unreal_exports` project.
  - Verify `.import`, `.md5`, UID cache, and editor resource visibility.

2. Concurrency:
  - Repeat import cycles with mixed importers (scene + texture).
  - Confirm no orphan `asset_start` without matching completion after successful run.

3. Regression:
  - Script class updates
  - Preview invalidation behavior
  - Generated files reload

4. Performance:
  - Compare wall-clock and CPU utilization before/after each phase.
  - Track throughput by importer class.

## Rollout Plan

1. Land Phase 1+2 with feature flags off by default for scene parallelization.
2. Enable scene threaded mode in dev branch with low cap (`2`) and watchdog.
3. Increase cap gradually based on telemetry and stability.
4. Keep serial escape hatch setting permanently available.

## Risk Register and Mitigations

1. Risk: Hidden main-thread dependency in importer code.
  - Mitigation: strict worker/commit separation and guarded scene mode.

2. Risk: UID/cache inconsistency under parallel completion.
  - Mitigation: all UID/cache mutation in single-thread commit phase.

3. Risk: Throughput regression from excessive commit work.
  - Mitigation: batch commit optimizations and minimal lock scope.

4. Risk: Starvation in worker pool.
  - Mitigation: do not saturate full pool; reserve at least one worker.

## Definition of Done

1. No reproducible stalls in repeated full-project reimports.
2. Scene imports show measurable parallelism (`threaded=1`, `threads_used>1`).
3. End-to-end import time improved in scene-heavy runs.
4. No correctness regressions in resource visibility, UID mapping, or cache behavior.
