# Ocean Camera LOD Parallelisation Options

This note tracks the current opportunities for broader parallelisation in the ocean camera LOD
path, and the measurements to compare each change against the dense reference.

## Current Summary Tables

These tables summarize the production-resolution `calm_reference` runs on `roni1`. Unless noted
otherwise, timings use ocean modifier resolution 64, render resolution 64, 4 Cycles samples, OPTIX,
and wall-clock seconds from `tests/python/ocean_camera_lod_variant_render.py`.

| Item | Value |
| --- | --- |
| Dense correctness baseline | Commit `b8175a8c27cd`, Blender dense path plus earlier ocean bugfixes |
| Baseline artifact | `/tmp/ocean_variant_res64_baseline_20260505_082243` |
| Current optimized build | `/home/xangma/repos/blender-git/build_linux_lod_changes_cuda/bin/blender` |
| Current optimized build hash | `8fbfc6780a0d` |
| Scenario | `calm_reference` |
| Ocean modifier resolution | 64 |
| Dense mesh | 16,785,409 verts, 16,777,216 faces |
| LOD eval mesh | 6,245,882 verts, 6,232,462 faces |
| LOD render mesh | 6,243,668 verts, 6,229,531 faces |

### Change List

| Change | Files | Main effect | Correctness status |
| --- | --- | --- | --- |
| LOD vertex displacement parallelization | `MOD_ocean.cc` | Makes LOD per-vertex displacement cheap, but not a major production-resolution bottleneck | LOD render difference unchanged |
| LOD breadth-frontier leaf validation | `MOD_ocean.cc` | LOD leaf build `10.57 s -> 2.10 s`; LOD eval `18.04 s -> 9.53 s` | LOD shape and render difference unchanged |
| LOD leaf balancing parallelization and topology scratch cleanup | `MOD_ocean.cc` | LOD leaf build `2.10 s -> 1.24 s`; LOD eval `9.53 s -> 8.81 s` | LOD shape and render difference unchanged |
| Dense displacement parallelization with scoped ocean read lock | `BKE_ocean.h`, `ocean.cc`, `MOD_ocean.cc` | Dense displacement `2.31 s -> 0.044 s`; dense eval `5.98 s -> 3.73 s` | Dense optimized render matches dense baseline exactly |
| LOD finest-grid dense fast path | `MOD_ocean.cc` | In dense-ceiling cases, adaptive geometry generation `3.64 s -> 1.44 s`; render-only `15.55 s -> 13.82 s` | Dense-ceiling LOD render still matches dense baseline exactly |
| LOD topology two-pass assembly and level-parallel split pyramid | `MOD_ocean.cc`, `ocean.cc` | `calm_reference` adaptive topology `0.95 s -> 0.31 s`; render-only `10.54 s -> 9.90 s` | LOD render difference unchanged except negligible stereo device noise |
| LOD point-domain custom normals | `MOD_ocean.cc` | `calm_reference` explicit eval finish `~2.8 s -> 0.057 s`; LOD eval `8.12 s -> 5.28 s` | LOD renders match previous LOD exactly in the curated matrix |
| Split normal computation parallelization | `ocean.cc` | `calm_reference` split simulation `0.868 s -> 0.649 s`; LOD eval `5.28 s -> 4.76 s` | LOD renders match previous LOD exactly in the curated matrix |
| Leaf hard-cap early exit | `MOD_ocean.cc` | `calm_reference` leaf selection `0.635 s -> 0.552 s`; LOD eval `4.76 s -> 4.71 s` | LOD renders match previous LOD exactly in the curated matrix |
| Dense-ceiling early completion and dense route | `MOD_ocean.cc` | `high_energy_dense_ceiling` LOD eval `6.43 s -> 4.94 s`; render-only `13.57 s -> 9.75 s` | Dense-ceiling render matches dense baseline exactly |
| Split moment accumulation and base write-back cleanup | `ocean.cc` | LOD split simulation about `0.04-0.06 s` faster; LOD eval about `0.09-0.15 s` faster | Dense renders match dense baseline exactly; LOD renders match previous LOD except negligible stereo noise |
| Benchmark and comparison helpers | `tests/python/ocean_camera_lod_variant_render.py`, `tests/python/ocean_camera_lod_compare_renders.py`, `tests/python/ocean_camera_lod_benchmark.py`, `tests/python/modules/ocean_camera_lod_metrics.py` | Separates dense baseline, dense optimized, and LOD optimized runs; records render deltas | Used for all artifact comparisons below |

### Eval Timing Progression

The eval path includes viewport/non-Cycles work. Before the point-domain custom-normal patch, LOD
explicit eval paid the large custom-normal conversion cost that Cycles render evaluation avoided.

| State | Artifact | Dense eval | Dense modifier | Dense displacement | LOD eval | LOD modifier | LOD leaf build | Notes |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Dense baseline reference | `/tmp/ocean_variant_res64_baseline_20260505_082243` | 5.76 | - | - | - | - | - | Original dense reference plus bugfixes |
| Before LOD leaf validation parallelization | `/tmp/ocean_variant_res64_eval_20260505_075848` | 6.04 | 4.29 | - | 18.04 | 15.78 | 10.57 | LOD dominated by sampled leaf validation |
| After LOD leaf validation parallelization | `/tmp/ocean_variant_res64_leaf_parallel_20260505_080118` | 5.98 | 4.25 | - | 9.53 | 7.23 | 2.10 | First large LOD win |
| After LOD leaf balancing parallelization | `/tmp/ocean_variant_res64_balance_parallel_20260505_093504` | 5.98 | 4.23 | - | 8.81 | 6.53 | 1.24 | Leaf balance substage `1.45 s -> 0.61 s` |
| After dense displacement parallelization | `/tmp/ocean_variant_res64_dense_parallel_fixed_20260505_103420` | 3.73 | 1.86-2.02 | 0.043-0.044 | 8.76 | 6.45 | 1.25 | Dense render equivalence restored after per-thread `OceanResult` fix |
| After topology/split follow-up | `/tmp/ocean_lod_topology_split_matrix_20260505_193528` | - | - | - | 8.12 | 5.66 | 1.24 | Adaptive topology direct mesh write and level-parallel split pyramid |
| After point-domain custom normals | `/tmp/ocean_lod_point_custom_normal_matrix_20260505_195516` | - | - | - | 5.28 | 3.08 | - | Finish/custom-normal cost down to `0.057 s`; settings/leaf selection about `1.26 s` |
| After split normal parallelization | `/tmp/ocean_lod_split_normals_parallel_20260505_213529` | - | - | - | 4.76 | 2.78 | - | Split simulation down to `0.649 s`; renders unchanged versus previous LOD |
| After leaf hard-cap early exit | `/tmp/ocean_lod_leaf_hard_cap_20260505_214822` | - | - | - | 4.71 | 2.72 | - | Leaf sample count `180,632 -> 24,196`; renders unchanged versus previous LOD |
| After dense-ceiling early completion | `/tmp/ocean_lod_dense_ceiling_cache_20260505_220441` | - | - | - | - | - | - | High-energy dense-ceiling eval `6.43 s -> 4.94 s`; dense-ceiling leaf build `1.398 s -> 0.371 s` |
| After split moment accumulation cleanup | `/tmp/ocean_lod_split_moments_parallel_20260505_230549` | - | - | - | 4.59 | 2.68 | - | `calm_reference` split simulation `0.648 s -> 0.601 s`; dense renders unchanged |

### Render Timing Progression

Render-only means `--repeat-eval 0`, so the timing reflects Cycles render evaluation without the
explicit viewport/non-Cycles eval pass.

| State | Artifact | Dense render after eval | LOD render after eval | Dense render-only | LOD render-only | Dense vs baseline | LOD vs baseline |
| --- | --- | ---: | ---: | ---: | ---: | --- | --- |
| Dense baseline reference | `/tmp/ocean_variant_res64_baseline_20260505_082243` | 9.22 | - | - | - | Reference | - |
| After LOD leaf validation parallelization | `/tmp/ocean_variant_res64_render_20260505_080157` | 8.37 | 9.49 | - | - | Zero error | RGB mean 0.0422 |
| After LOD leaf balancing parallelization | Dense: `/tmp/ocean_variant_res64_render_only_20260505_093240`<br>LOD: `/tmp/ocean_variant_res64_balance_render_only_20260505_093539` | - | - | 14.59 | 10.53 | Zero error | RGB mean 0.0422 |
| After dense displacement parallelization | `/tmp/ocean_variant_res64_dense_parallel_fixed_20260505_103420` | 6.12 | 8.45 | - | - | Zero error | RGB mean 0.0422 |
| After dense displacement parallelization, render-only | `/tmp/ocean_variant_res64_dense_parallel_render_only_fixed_20260505_103513` | - | - | 9.77 | 10.51 | Zero error | RGB mean 0.0422 |
| After topology/split follow-up, render-only | `/tmp/ocean_lod_topology_split_matrix_20260505_193528` | - | - | - | 9.90 | - | RGB mean 0.0422 |
| After point-domain custom normals | `/tmp/ocean_lod_point_custom_normal_matrix_20260505_195516` | - | 7.65 | - | - | - | RGB mean 0.0422; zero error vs previous LOD |
| After split normal parallelization | `/tmp/ocean_lod_split_normals_parallel_20260505_213529` | - | 7.28 | - | - | - | RGB mean 0.0422; zero error vs previous LOD |
| After leaf hard-cap early exit | `/tmp/ocean_lod_leaf_hard_cap_20260505_214822` | - | 7.09 | - | - | - | RGB mean 0.0422; zero error vs previous LOD |
| After dense-ceiling early completion | Matrix: `/tmp/ocean_lod_dense_ceiling_cache_20260505_220441`<br>Render-only: `/tmp/ocean_lod_dense_ceiling_render_only_20260505_220619` | - | - | - | 9.75 | - | High-energy dense-ceiling zero error versus dense baseline |
| After split moment accumulation cleanup | `/tmp/ocean_lod_split_moments_parallel_20260505_230549` | - | 7.04 | - | - | Zero error | RGB mean 0.0422; zero error vs previous LOD except negligible stereo noise |

### Render Error Versus Dense Baseline

The latest dense optimized render is exactly equivalent to dense baseline. The latest LOD optimized
render has the same measured information loss as before the later speedups.

| Candidate | RGB mean / p95 / max | Luminance mean / p95 / max | Luminance-gradient mean / p95 / max |
| --- | --- | --- | --- |
| Dense optimized | 0.0000 / 0.0000 / 0.0000 | 0.0000 / 0.0000 / 0.0000 | 0.0000 / 0.0000 / 0.0000 |
| LOD optimized | 0.0422 / 0.1229 / 0.1686 | 0.0426 / 0.1229 / 0.1702 | 0.0148 / 0.0422 / 0.0647 |

### Current Takeaways

| Takeaway | Detail |
| --- | --- |
| Biggest implemented win | Dense displacement parallelization with scoped ocean read lock |
| Biggest LOD implemented win | Breadth-frontier leaf validation and point-domain custom normals, followed by split normal parallelization, parallel leaf balancing, and two-pass topology assembly |
| Current dense status | Faster and render-identical to dense baseline |
| Current LOD status | Explicit eval is much closer after removing the custom-normal conversion bottleneck; render-only still clearly wins only when geometry reduction is very high |
| Next likely LOD targets | Leaf-balance locality, settings cache reuse, and render-path cache/sync reductions |

### Curated Scenario Matrix

This matrix uses all five curated scenarios at ocean modifier resolution 64. Each scenario was run
through dense baseline, dense optimized, and LOD optimized variants on `roni1`.

Artifact root: `/tmp/ocean_variant_res64_matrix_20260505_153650`

| Scenario | Dense baseline eval | Dense optimized eval | LOD optimized eval | Dense optimized render-only | LOD optimized render-only | LOD vertex reduction | Dense RGB mean/max | LOD RGB mean/p95/max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| `calm_reference` | 5.93 | 3.78 | 8.65 | 9.68 | 10.54 | 62.8% | 0.0000 / 0.0000 | 0.0422 / 0.1229 / 0.1686 |
| `high_energy_dense_ceiling` | 6.16 | 3.85 | 17.16 | 9.71 | 15.55 | 0.0% | 0.0000 / 0.0000 | 0.0000 / 0.0000 / 0.0000 |
| `grazing_light_adversarial` | 6.22 | 3.88 | 4.76 | 10.03 | 7.95 | 96.5% | 0.0000 / 0.0000 | 0.0000 / 0.0000 / 0.0013 |
| `temporal_camera_move` | 5.83 | 3.68 | 9.07 | 9.86 | 10.65 | 60.1% | 0.0000 / 0.0000 | 0.0487 / 0.1216 / 0.1412 |
| `stereo_dataset_valid` | 5.99 | 3.73 | 8.59 | 9.83 | 10.50 | 62.3% | 0.0000 / 0.0000 | 0.0357 / 0.1072 / 0.1595 |

The matrix timings are operation wall times from the benchmark scripts. They time the explicit
depsgraph evaluation and render operation, not full Blender process startup/shutdown. The matrix run
was not wrapped in `/usr/bin/time`, so full process wall time is not available for these rows.

| Scenario | Dense eval wall change | Dense eval speedup | LOD eval wall vs dense opt | Dense render-only wall | LOD render-only wall | LOD render-only vs dense opt |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `calm_reference` | 5.93 -> 3.78 (-2.15) | 1.57x | 8.65 vs 3.78 (2.29x) | 9.68 | 10.54 (+0.86) | 1.09x |
| `high_energy_dense_ceiling` | 6.16 -> 3.85 (-2.31) | 1.60x | 17.16 vs 3.85 (4.46x) | 9.71 | 15.55 (+5.85) | 1.60x |
| `grazing_light_adversarial` | 6.22 -> 3.88 (-2.34) | 1.60x | 4.76 vs 3.88 (1.23x) | 10.03 | 7.95 (-2.08) | 0.79x |
| `temporal_camera_move` | 5.83 -> 3.68 (-2.15) | 1.58x | 9.07 vs 3.68 (2.46x) | 9.86 | 10.65 (+0.79) | 1.08x |
| `stereo_dataset_valid` | 5.99 -> 3.73 (-2.25) | 1.60x | 8.59 vs 3.73 (2.30x) | 9.83 | 10.50 (+0.67) | 1.07x |

Scenario-level observations:

- Dense optimized renders match dense baseline exactly for all curated scenarios.
- Dense optimized eval is consistently about 1.6x faster than dense baseline eval.
- Current LOD render-only beats optimized dense only in `grazing_light_adversarial`, where vertex
  reduction is about 96.5% and render error is effectively zero.
- `high_energy_dense_ceiling` was the clearest LOD-path problem in this matrix: LOD emits
  dense-sized geometry, has zero image error, but is much slower than optimized dense. A follow-up
  finest-grid fast path now avoids adaptive topology generation for this case, but it still pays
  LOD settings/split-simulation overhead.
- The 60-63% reduction scenarios (`calm_reference`, `temporal_camera_move`, and
  `stereo_dataset_valid`) are still slightly slower than optimized dense in render-only timing, so
  topology assembly and split-pyramid simulation remain the likely LOD-specific targets.

### Dense-Ceiling Fast-Path Follow-Up

The scenario matrix showed that `high_energy_dense_ceiling` refined every cell to the finest level.
LOD therefore produced dense-sized geometry with zero render error, while still paying adaptive
topology generation. A follow-up patch detects when the accepted leaves cover the finest grid and
uses the existing dense LOD mesh generator instead of adaptive topology assembly.

Artifact root: `/tmp/ocean_variant_res64_dense_ceiling_fastpath_20260505_154519`

| Scenario | LOD eval before | LOD eval after | LOD render-only before | LOD render-only after | Geometry generation before | Geometry generation after | Render error |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `high_energy_dense_ceiling` | 17.16 | 15.14 | 15.55 | 13.82 | 3.64 | 1.44 | Zero RGB/luminance/gradient error |

Remaining dense-ceiling cost is mostly outside adaptive topology: leaf/settings selection is about
1.59 s, split simulation is about 1.29 s, and viewport/non-Cycles custom-normal conversion is about
7.77 s for explicit eval. The render-only path avoids custom-normal conversion but still pays
settings, split simulation, and LOD mesh sync overhead.

### Topology And Split Follow-Up

This patch changes adaptive topology generation from a single serial assembly loop into a
deterministic two-pass path:

- Build per-leaf corner counts and corner coordinates in parallel.
- Assign dense-grid vertex indices serially in leaf order to preserve deterministic first-use vertex
  ordering.
- Write final mesh face offsets, corner vertices, and face attributes directly into the result mesh
  buffers instead of building and copying large temporary face vectors.
- Build independent reduced split-pyramid levels in parallel while keeping each level's FFT and
  moment-reduction order unchanged.

Artifact root: `/tmp/ocean_lod_topology_split_matrix_20260505_193528`

| Scenario | LOD eval before | LOD eval after | LOD render-only before | LOD render-only after | Adaptive topology after | Split simulation after | Output vs previous LOD |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `calm_reference` | 8.65 | 8.12 | 10.54 | 9.90 | 0.313 | 0.923 | Zero RGB/luminance/gradient error |
| `high_energy_dense_ceiling` | 15.14 | 13.76 | 13.82 | 13.57 | Dense fast path | 1.186 | Zero RGB/luminance/gradient error |
| `grazing_light_adversarial` | 4.76 | 4.48 | 7.95 | 7.71 | 0.036 | 1.189 | Zero RGB/luminance/gradient error |
| `temporal_camera_move` | 9.07 | 8.40 | 10.65 | 9.86 | 0.339 | 0.929 | Zero RGB/luminance/gradient error |
| `stereo_dataset_valid` | 8.59 | 8.36 | 10.50 | 9.71 | 0.319 | 0.925 | RGB mean `1.6e-7`, max `0.0013` |

The dense-baseline information-loss metrics stayed unchanged for the curated scenarios. For
`calm_reference`, adaptive topology generation went from about 1.37 s total with 0.95 s in topology
assembly to about 0.73 s total with 0.31 s in topology assembly. After this patch, the remaining
explicit-eval LOD cost for the 60-63% reduction cases was dominated by viewport/non-Cycles
custom-normal conversion, leaf/settings selection, and split simulation rather than topology
assembly.

### Point-Domain Custom Normal Follow-Up

This patch stores generated LOD geometry-band normals as a point-domain `float3 custom_normal`
attribute instead of sending them through the generic corner custom-normal encoder. The ocean
modifier already computes one normalized normal per vertex, and mesh normal evaluation can consume
point-domain custom normals directly.

Artifact root: `/tmp/ocean_lod_point_custom_normal_matrix_20260505_195516`

| Scenario | LOD eval before | LOD eval after | LOD modifier after | Finish/custom-normal after | Render after eval | Output vs previous LOD |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `calm_reference` | 8.12 | 5.28 | 3.08 | 0.057 | 7.65 | Zero RGB/luminance/gradient error |
| `high_energy_dense_ceiling` | 13.76 | 7.09 | 4.59 | 0.114 | 9.83 | Zero RGB/luminance/gradient error |
| `grazing_light_adversarial` | 4.48 | 4.03 | 1.59 | 0.011 | 5.18 | Zero RGB/luminance/gradient error |
| `temporal_camera_move` | 8.40 | 5.39 | 3.18 | 0.060 | 7.92 | Zero RGB/luminance/gradient error |
| `stereo_dataset_valid` | 8.36 | 5.33 | 3.12 | 0.057 | 7.55 | Zero RGB/luminance/gradient error |

Dense-baseline information-loss metrics stayed unchanged. The dense-ceiling case still has zero
render error versus dense baseline, and the 60-63% geometry-reduction scenarios keep the same
measured LOD-vs-dense render deltas as before this patch.

### Split Normal/Moment Follow-Up

This patch parallelizes split-level normal computation while keeping moment accumulation in the
original serial `x/y` order. The expensive derivative, cross-product, and normalization work is
independent per sample; the serial accumulation keeps leaf decisions independent of thread
scheduling.

Artifact root: `/tmp/ocean_lod_split_normals_parallel_20260505_213529`

| Scenario | LOD eval before | LOD eval after | Split simulation before | Split simulation after | LOD modifier after | Output vs previous LOD |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `calm_reference` | 5.28 | 4.76 | 0.868 | 0.649 | 2.78 | Zero RGB/luminance/gradient error |
| `high_energy_dense_ceiling` | 7.09 | 6.61 | 1.143 | 0.910 | 4.33 | Zero RGB/luminance/gradient error |
| `grazing_light_adversarial` | 4.03 | 3.60 | 1.119 | 0.912 | 1.37 | Zero RGB/luminance/gradient error |
| `temporal_camera_move` | 5.39 | 4.95 | 0.874 | 0.658 | 2.96 | Zero RGB/luminance/gradient error |
| `stereo_dataset_valid` | 5.33 | 4.91 | 0.874 | 0.674 | 2.90 | Zero RGB/luminance/gradient error |

Dense-baseline information-loss metrics stayed unchanged. The remaining repeated LOD costs are now
mostly settings/leaf selection, dense-ceiling LOD overhead, and the still-serial moment accumulation
portion of split runtime.

### Leaf Hard-Cap Early-Exit Follow-Up

This patch stops per-leaf validation sampling as soon as a sampled max error exceeds the same hard
cap used by the final tolerance check. It can only make a leaf split sooner after failure is already
guaranteed; it cannot accept a leaf that the previous full-sampling path would reject. The profile
now logs `hard_cap_exits` for the settings stage.

Artifact root: `/tmp/ocean_lod_leaf_hard_cap_20260505_214822`

| Scenario | LOD eval before | LOD eval after | Leaf selection before | Leaf selection after | Region samples after | Output vs previous LOD |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `calm_reference` | 4.76 | 4.71 | 0.635 | 0.552 | 24,196 | Zero RGB/luminance/gradient error |
| `high_energy_dense_ceiling` | 6.61 | 6.43 | 1.574 | 1.359 | 63,919 | Zero RGB/luminance/gradient error |
| `grazing_light_adversarial` | 3.60 | 3.66 | 0.130 | 0.131 | 938 | Zero RGB/luminance/gradient error |
| `temporal_camera_move` | 4.95 | 4.90 | 0.688 | 0.586 | 25,605 | Zero RGB/luminance/gradient error |
| `stereo_dataset_valid` | 4.91 | 4.73 | 0.691 | 0.582 | 23,526 | Zero RGB/luminance/gradient error |

The win is modest but useful in cases with many rejected leaves. It mostly removes unnecessary
sample work inside leaf selection; balance remains the larger settings cost for the 60-63% reduction
scenarios.

### Dense-Ceiling Early-Completion Follow-Up

This patch marks a dense ceiling during leaf selection when all level-1 leaves are already proven to
split and no coarser leaves have been accepted. It stops before emitting the full dense leaf set, then
routes the remainder of the modifier through the optimized dense generated/displacement path while
keeping the result in the existing camera-LOD modifier cache.

Matrix artifact root: `/tmp/ocean_lod_dense_ceiling_cache_20260505_220441`

Focused high-energy render-only artifact:
`/tmp/ocean_lod_dense_ceiling_render_only_20260505_220619`

| Scenario | LOD eval before | LOD eval after | Render after eval before | Render after eval after | Dense-ceiling route | Output vs previous LOD |
| --- | ---: | ---: | ---: | ---: | --- | --- |
| `calm_reference` | 4.71 | 4.69 | 7.09 | 7.16 | No | Zero RGB/luminance/gradient error |
| `high_energy_dense_ceiling` | 6.43 | 4.94 | 9.23 | 7.52 | Yes | Zero RGB/luminance/gradient error |
| `grazing_light_adversarial` | 3.66 | 3.64 | 4.60 | 4.65 | No | Zero RGB/luminance/gradient error |
| `temporal_camera_move` | 4.90 | 4.84 | 7.33 | 7.37 | No | Zero RGB/luminance/gradient error |
| `stereo_dataset_valid` | 4.73 | 4.80 | 7.01 | 6.90 | No | RGB mean `1.6e-7`, max `0.0013` |

For `high_energy_dense_ceiling`, the settings profile switched to `selection=dense_ceiling`,
`dense_ceiling=1`, and modifier mode `dense_reference` with `use_camera_lod_mesh=0`. The focused
render-only run was `9.75 s` and had zero RGB, luminance, and luminance-gradient error versus dense
baseline.

### Split Moment Accumulation Follow-Up

This patch parallelizes split moment accumulation in deterministic x-row chunks and removes the
redundant copy of split level 0 displacement back onto the main dense simulation buffers. Moment
chunks are combined in chunk order, so results do not depend on thread scheduling. Dense renders
still matched dense baseline exactly across the curated matrix.

Artifact root: `/tmp/ocean_lod_split_moments_parallel_20260505_230549`

| Scenario | LOD eval before | LOD eval after | Split simulation before | Split simulation after | Render after eval after | Output vs previous LOD |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `calm_reference` | 4.688 | 4.594 | 0.648 | 0.601 | 7.042 | Zero RGB/luminance/gradient error |
| `high_energy_dense_ceiling` | 4.941 | 4.823 | 0.904 | 0.859 | 7.417 | Zero RGB/luminance/gradient error |
| `grazing_light_adversarial` | 3.644 | 3.492 | 0.900 | 0.863 | 4.525 | Zero RGB/luminance/gradient error |
| `temporal_camera_move` | 4.844 | 4.752 | 0.659 | 0.602 | 7.218 | Zero RGB/luminance/gradient error |
| `stereo_dataset_valid` | 4.804 | 4.703 | 0.654 | 0.602 | 6.765 | RGB mean `1.6e-7`, max `0.0013` |

The dense optimized render remained zero-error versus dense baseline for RGB, luminance, and
luminance-gradient metrics in all five scenarios. The LOD-vs-dense information-loss metrics stayed
effectively unchanged.

### Pruning Experiments Not Retained

Two follow-up pruning ideas were implemented and tested after the dense-ceiling route, but were not
kept because the measured timings were neutral or slower.

Root-frontier pruning classified coarsest roots with the existing relevance test and appended
irrelevant roots directly as accepted leaves before the sampled split frontier. It preserved render
output, but did not materially reduce wall time because the skipped root test was already cheap and
parallel.

Artifact root: `/tmp/ocean_lod_root_frontier_20260505_221351`

| Scenario | LOD eval before | LOD eval with pruning | Leaf build before | Leaf build with pruning | Output vs previous LOD |
| --- | ---: | ---: | ---: | ---: | --- |
| `calm_reference` | 4.688 | 4.730 | 1.169 | 1.158 | Zero RGB error |
| `high_energy_dense_ceiling` | 4.941 | 4.905 | 0.371 | 0.377 | Zero RGB error |
| `grazing_light_adversarial` | 3.644 | 3.622 | 0.307 | 0.311 | Zero RGB error |
| `temporal_camera_move` | 4.844 | 4.844 | 1.239 | 1.229 | Zero RGB error |
| `stereo_dataset_valid` | 4.804 | 4.783 | 1.209 | 1.211 | Zero RGB error |

Split-level count pruning limited generated camera LOD simulation to the quadtree split levels that
the emitted LOD mesh can sample. Dense renders still matched dense baseline exactly, and LOD renders
stayed equivalent to the previous LOD output apart from normal device noise, but simulation timings
were slightly worse in this matrix. The reduced split levels beyond the generated quadtree are not
the current bottleneck.

Artifact root: `/tmp/ocean_lod_split_level_prune_20260505_222615`

| Scenario | LOD eval before | LOD eval with pruning | Split simulation before | Split simulation with pruning | Dense vs baseline |
| --- | ---: | ---: | ---: | ---: | --- |
| `calm_reference` | 4.688 | 4.806 | 0.648 | 0.698 | Zero RGB/luminance/gradient error |
| `high_energy_dense_ceiling` | 4.941 | 5.061 | 0.904 | 0.967 | Zero RGB/luminance/gradient error |
| `grazing_light_adversarial` | 3.644 | 3.716 | 0.900 | 0.976 | Zero RGB/luminance/gradient error |
| `temporal_camera_move` | 4.844 | 4.948 | 0.659 | 0.711 | Zero RGB/luminance/gradient error |
| `stereo_dataset_valid` | 4.804 | 4.932 | 0.654 | 0.702 | Zero RGB/luminance/gradient error |

## Baseline and Metrics

Production runs should use ocean modifier resolution 64 or higher. Lower resolutions are only useful
for smoke tests and isolating overhead.

Use `tests/python/ocean_camera_lod_benchmark.py` to compare:

- Dense evaluation time versus camera LOD evaluation time.
- Geometry reduction and vertex/face counts.
- Dense-reference geometry error: position, reprojection, depth, and geometric normal.
- Cycles render difference: RGB, luminance, and luminance-gradient error.
- `BLENDER_OCEAN_CAMERA_LOD_PROFILE` stages: simulation, settings, leaf build, geometry generation,
  displacement, finish, and cache hits.

Run the same scenario set before and after each implementation so speed changes are tied to the
dense-reference error report.

Use `--ocean-resolution N` to run the curated scenarios at a higher ocean modifier resolution
without patching the scenario definitions.

Use `--repeat-eval 0` with `ocean_camera_lod_variant_render.py` when measuring the Cycles render
path by itself. The explicit eval step uses the viewport/non-Cycles modifier context and therefore
includes custom-normal conversion that Cycles render evaluation avoids.

For production comparisons, keep three outputs distinct:

1. Dense baseline: Blender original dense path plus the earlier ocean bugfix commits. This is the
   correctness reference.
2. Dense optimized: only needed when an optimization touches shared or dense-path code. It must
   render-identically to dense baseline, apart from normal device/render noise.
3. LOD optimized: compare speed against dense baseline and compare render information loss against
   dense baseline.

Dense-path optimizations are in scope when they produce large speedups. Any shared or dense-path
change must be checked by rendering dense optimized against dense baseline and confirming zero image
error for RGB, luminance, and luminance-gradient metrics.

Use `tests/python/ocean_camera_lod_variant_render.py` to render one variant from a given binary:

```
blender --background --factory-startup \
  --python tests/python/ocean_camera_lod_variant_render.py -- \
  --outdir /tmp/ocean_reference/calm_reference \
  --scenario calm_reference \
  --variant dense \
  --ocean-resolution 64 \
  --device OPTIX \
  --samples 4 \
  --resolution 64
```

Then compare candidate images to dense baseline with:

```
blender --background --factory-startup \
  --python tests/python/ocean_camera_lod_compare_renders.py -- \
  --reference /tmp/ocean_reference/calm_reference/calm_reference_dense.png \
  --candidate /tmp/ocean_lod/calm_reference/calm_reference_lod.png \
  --outdir /tmp/ocean_compare/calm_reference \
  --label lod_vs_dense_baseline
```

## Options

1. Parallelize camera-LOD vertex displacement and metadata writes.
   The per-vertex camera LOD mesh path samples immutable split levels and writes disjoint vertex
   positions/attributes. This is the lowest-risk first target. Keep split-debug accumulation serial
   unless it gets explicit reductions. First pass implemented in `MOD_ocean.cc`.

2. Parallelize split pyramid construction in `ocean.cc`.
   The main simulation already uses task pools, but the split pyramid runs serially after the main
   FFT work. The reduced-level spectrum preparation, FFT output copies, and normal/moment reductions
   are good candidates. Cross-level FFT execution needs care around FFTW plan/thread behavior.

3. Parallelize dense vertex displacement.
   Dense displacement is independent per vertex, but calling `BKE_ocean_eval_uv` from each vertex
   previously meant one read-lock acquire/release per sample. A scoped ocean read API lets the
   modifier hold the read lock once and parallelize the dense per-vertex displacement loop while
   using the same interpolation code. Implemented in `ocean.cc`, `BKE_ocean.h`, and `MOD_ocean.cc`.

4. Parallelize adaptive leaf validation by breadth-first frontier.
   Leaf acceptance is currently stack based. A level/frontier pass can evaluate candidate leaves in
   parallel, then serially append accepted leaves and children. First pass implemented in
   `MOD_ocean.cc`.

5. Parallelize adaptive leaf balancing.
   The balance pass rebuilds a dense owner-cell map and checks leaf neighbors. Those writes are
   disjoint and the neighbor scan is read-only, so both can run in parallel before serially
   compacting the next leaf list. First pass implemented in `MOD_ocean.cc`.

6. Parallelize adaptive topology assembly with a two-pass layout.
   The current builder used shared vectors and a dense vertex map. Implemented as a deterministic
   two-pass count/fill design that keeps first-use vertex ordering stable while writing final mesh
   buffers directly.

7. Avoid generic corner custom-normal conversion for LOD eval.
   The camera LOD path generates one normalized normal per vertex. Implemented by storing those
   normals as a point-domain `custom_normal` attribute instead of encoding them through the
   corner-domain custom-normal path.

## Correctness Notes Before Benchmarking

- Topology cache validity should be reviewed before trusting time-varying results, because topology
  selection samples current split geometry while the topology cache key currently ignores frame/time.
- Versioning should initialize all newly added LOD fields for existing files, especially
  `lod_pixel_error`, so old files do not inherit an unintentionally strict tolerance.

## Measurement Log

### Vertex displacement parallelization, pass 1

Build:

- Host: `roni1`
- Build dir: `/home/xangma/repos/blender-git/build_linux_lod_changes_cuda`
- Devices detected by benchmark: `CPU`, `CUDA`, `OPTIX`, `OPTIX-OSL`
- Binary built successfully: `bin/blender`

Curated benchmark at scenario resolution 6:

- Artifact dir: `/tmp/ocean_lod_parallel_20260505_073957`
- Command shape: `--scenarios all --devices OPTIX --samples 4 --resolution 64 --repeat-eval 5 --repeat-render 2`
- Result: LOD evaluation remained slower than dense in all five cases.
- Geometry reduction ranged from 0% in the high-energy dense-ceiling case to about 53%.
- Render differences stayed very small, but `grazing_light_adversarial` still failed the position
  error threshold.

Forced ocean modifier resolution 10:

- Artifact dir: `/tmp/ocean_lod_parallel_res10_20260505_074107`
- Scenarios: `calm_reference`, `temporal_camera_move`, `stereo_dataset_valid`
- Future command shape:
  `--scenarios calm_reference,temporal_camera_move,stereo_dataset_valid --ocean-resolution 10`
- Dense mesh: 10,201 verts, 10,000 faces.
- LOD meshes: about 4,600 to 4,840 verts, about 4,400 to 4,650 faces.
- Evaluation result: LOD was still slower, at about 0.48x to 0.58x dense speed.
- Render result: RGB/luminance differences remained low, but the strict position-error max
  threshold failed at about 1.88 m versus the 1.50 m threshold.

Focused C++ profile at forced resolution 10 with `BLENDER_OCEAN_CAMERA_LOD_PROFILE=1`:

- LOD displacement is now cheaper than dense displacement: about 0.00034 s to 0.00038 s for LOD
  versus about 0.00064 s to 0.00113 s for dense in the focused runs.
- LOD total modifier time is still higher: about 0.0076 s to 0.0080 s versus about 0.0044 s to
  0.0053 s for dense.
- The larger remaining costs are settings and leaf build, adaptive topology/finalize, and finishing
  custom normals. The next parallelization pass should target leaf validation or topology/finalize
  work before doing more work on vertex displacement.

### Production-resolution comparison, ocean resolution 64

Dense baseline:

- Baseline commit: `b8175a8c27cd` (`Fix: Correct index usage in ocean normal computation`)
- Worktree: `/home/xangma/repos/blender-git/blender_dense_baseline`
- Build dir: `/home/xangma/repos/blender-git/build_linux_dense_baseline_cuda`

Evaluation-only `calm_reference`, before parallel leaf validation:

- Artifact dir: `/tmp/ocean_variant_res64_eval_20260505_075848`
- Dense baseline shape from current build: 16,785,409 verts, 16,777,216 faces.
- LOD shape: 6,245,882 verts, 6,232,462 faces.
- Dense eval wall time: about 6.04 s; dense modifier profile: about 4.29 s.
- LOD eval wall time: about 18.04 s; LOD modifier profile: about 15.78 s.
- LOD leaf build: about 10.57 s, dominated by sampled region stats.

Evaluation-only `calm_reference`, after breadth-frontier parallel leaf validation:

- Artifact dir: `/tmp/ocean_variant_res64_leaf_parallel_20260505_080118`
- LOD shape unchanged: 6,245,882 verts, 6,232,462 faces.
- Dense eval wall time: about 5.98 s; dense modifier profile: about 4.25 s.
- LOD eval wall time: about 9.53 s; LOD modifier profile: about 7.23 s.
- LOD leaf build dropped from about 10.57 s to about 2.10 s.
- LOD is still slower than dense for this case; remaining large costs are custom-normal finish and
  adaptive topology/finalize.

Render comparison, `calm_reference`, ocean resolution 64, render resolution 64, 4 samples, OPTIX:

- Current dense/LOD artifact dir: `/tmp/ocean_variant_res64_render_20260505_080157`
- Dense baseline artifact dir: `/tmp/ocean_variant_res64_baseline_20260505_082243`
- Dense baseline: eval about 5.76 s, render about 9.22 s.
- Current dense: eval about 5.97 s, render about 8.37 s.
- Current LOD: eval about 9.65 s, render about 9.49 s.
- Current dense versus dense baseline render error was exactly zero for RGB, luminance, and gradient
  metrics at this resolution/sample count.
- Current LOD versus dense baseline render error:
  - RGB absolute mean 0.0422, p95 0.1229, max 0.1686.
  - Luminance absolute mean 0.0426, p95 0.1229, max 0.1702.
  - Luminance-gradient mean 0.0148, p95 0.0422, max 0.0647.

### Production-resolution follow-up, leaf balancing and topology scratch pass

Implemented in `MOD_ocean.cc`:

- Parallel owner-cell map fill in topology generation.
- Inline-vector scratch buffers for per-edge and per-face topology assembly.
- Parallel normal override loop and use of the normalized custom-normal API for the already
  normalized LOD custom-normal buffer.
- Parallel owner-cell map fill and neighbor checks in adaptive leaf balancing.

Evaluation-only `calm_reference`, ocean resolution 64:

- Artifact dir after topology scratch pass only:
  `/tmp/ocean_variant_res64_topology_inline_20260505_093112`
- LOD eval wall time: about 9.75 s; LOD modifier profile: about 7.39 s.
- Geometry generation moved only slightly: about 1.36 s total, with owner-map fill about 0.007 s.
- This pass alone did not materially change the end-to-end result.

Evaluation-only after parallel leaf balancing:

- Artifact dir: `/tmp/ocean_variant_res64_balance_parallel_20260505_093504`
- Dense eval wall time: about 5.98 s; dense modifier profile: about 4.23 s.
- LOD eval wall time: about 8.81 s; LOD modifier profile: about 6.53 s.
- LOD leaf build dropped from about 2.10 s after parallel validation to about 1.24 s.
- Leaf balance dropped from about 1.45 s to about 0.61 s.
- LOD shape unchanged versus the evaluation baseline: 6,245,882 verts, 6,232,462 faces.
- The eval path is still slower than dense because it includes non-Cycles custom-normal conversion
  of about 2.8 s.

Render-only Cycles comparison, `calm_reference`, ocean resolution 64, render resolution 64,
4 samples, OPTIX:

- Dense current render-only artifact dir:
  `/tmp/ocean_variant_res64_render_only_20260505_093240`
- LOD render-only artifact dir:
  `/tmp/ocean_variant_res64_balance_render_only_20260505_093539`
- Current dense render-only wall time: about 14.59 s.
- Current LOD render-only wall time after parallel leaf balancing: about 10.53 s.
- LOD render-path modifier profile: about 3.73 s, with leaf build about 1.25 s and geometry
  generation about 1.41 s. Cycles render context avoids the custom-normal finish path.
- Current dense versus dense baseline render error remained exactly zero.
- Current LOD versus dense baseline render error was unchanged from the earlier render comparison:
  RGB mean 0.0422, p95 0.1229, max 0.1686; luminance mean 0.0426, p95 0.1229, max 0.1702;
  luminance-gradient mean 0.0148, p95 0.0422, max 0.0647.

Full eval-then-render LOD sample after parallel leaf balancing:

- Artifact dir: `/tmp/ocean_variant_res64_balance_full_lod_20260505_093702`
- LOD eval wall time: about 8.44 s.
- LOD render wall time after the explicit eval: about 8.45 s.
- Render difference versus dense baseline stayed unchanged from the render-only comparison.

### Production-resolution dense displacement parallelization

Implemented in `ocean.cc`, `BKE_ocean.h`, and `MOD_ocean.cc`:

- Added `OceanRuntimeReadScope` and `BKE_ocean_eval_uv_in_scope`.
- Refactored `BKE_ocean_eval_uv` so the scoped and unscoped APIs use the same locked sampling
  helper.
- The dense modifier path now holds the ocean read lock once and parallelizes non-cached dense
  vertex displacement.
- Cached dense ocean reads remain serial.
- Per-vertex `OceanResult` scratch is local on the parallel dense path; the initial attempt reused
  the old shared scratch variable and produced nonzero render differences, so that version was
  rejected.

Evaluation and render, `calm_reference`, ocean resolution 64, render resolution 64, 4 samples,
OPTIX:

- Artifact dir: `/tmp/ocean_variant_res64_dense_parallel_fixed_20260505_103420`
- Dense eval wall time: about 3.73 s.
- Dense render wall time after explicit eval: about 6.12 s.
- Dense modifier profile: about 1.86 s to 2.02 s.
- Dense displacement profile: about 0.043 s to 0.044 s, down from about 2.31 s before this pass.
- Current dense optimized versus dense baseline render error was exactly zero for RGB, luminance,
  and luminance-gradient metrics.
- LOD eval in the same run: about 8.76 s; LOD render after explicit eval: about 8.45 s.
- LOD render difference versus dense baseline stayed unchanged:
  RGB mean 0.0422, p95 0.1229, max 0.1686; luminance mean 0.0426, p95 0.1229, max 0.1702;
  luminance-gradient mean 0.0148, p95 0.0422, max 0.0647.

Render-only comparison after dense displacement parallelization:

- Artifact dir: `/tmp/ocean_variant_res64_dense_parallel_render_only_fixed_20260505_103513`
- Current dense render-only wall time: about 9.77 s.
- Current LOD render-only wall time: about 10.51 s.
- Current dense optimized versus dense baseline render error remained exactly zero.
- Current LOD versus dense baseline render error remained unchanged from the earlier LOD
  comparisons.
- This dense win raises the performance bar for LOD: for `calm_reference` at resolution 64, LOD is
  no longer faster than optimized dense in render-only timing, despite unchanged LOD information-loss
  metrics.
