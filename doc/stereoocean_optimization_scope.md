# Ocean and Cycles optimization scope

This manifest labels every commit in the optimization series after
`91bd7f03eddfc17d6fa1b170e1f54512274be3ea`, through
`288a3701d5de6f2d5d54130875ccfc9b582ecdf9`. Original commit IDs are retained
because the qualification reports and binary provenance refer to them.

**General Blender** means the implementation applies to ordinary Blender Ocean
or Cycles workloads, without StereoOcean application code or dataset settings.
It does not imply upstream acceptance or qualification on every device/scene.
Some implementations reuse APIs supplied by this fork and would need adapting
when ported to upstream Blender.

**StereoOcean fork** means the change depends on this fork's camera-LOD feature
or its status UI. It can benefit other users of that feature, but is not an
optimization of a feature already available in stock Blender.

| Commit | Scope | Change and limits |
| --- | --- | --- |
| `26a941958b58` | General Blender / Ocean | Sample foam and spray once per vertex, scatter to corners, and hold one read scope per sampling pass. Reuses the fork's read-scope API; retains the public sampler's locking. |
| `c6775de3b503` | General Blender / Ocean, experimental | CUDA dense displacement and foam/compression/spray sampling. CPU FFTs remain. Requires build option `WITH_OCEAN_CUDA` and runtime `BLENDER_OCEAN_CUDA=1`; build default is OFF. Camera LOD, baked data, small meshes and unavailable GPUs use CPU sampling. |
| `56dcd34bdf89` | General Blender / Ocean | Reuse matching live dense GENERATE evaluations across dependency graphs, including exact sampling-field snapshots. Camera LOD, baked/DISPLACE paths, vertex groups and original-application evaluation bypass reuse. Default enabled; `BLENDER_OCEAN_REUSE=0` is a diagnostic comparison switch. |
| `d904a931d972` | General Blender / Cycles, diagnostics | Log geometry BVH build/refit and scene BVH build/upload timings. This is instrumentation, not a separate speedup. |
| `db54390642dc` | StereoOcean fork, supporting fix | Refresh the fork's Ocean viewport status on dense-cache hits. Supports `56dcd34bdf89`; not a separate speedup. |
| `51abc3a73dc1` | General Blender / Cycles | Introduce persistent offline refits for unchanged plain-mesh connectivity. Historical opt-in prototype; requires the following normal, default-policy and Embree fixes as a group. |
| `f6d5fa2c0299` | General Blender / Cycles, supporting fix | Match static-BVH normal handling for identity meshes on the retained dynamic path, avoiding redundant normalization. Part of the refit change, not an independent optimization. |
| `b095d36c6741` | General Blender / Cycles | Enable eligible offline refits automatically with the existing Persistent Data setting; remove the experimental refit switch and retain static fallback sessions. |
| `bebd2d50af46` | General Blender / Cycles, supporting fix | Refresh relocated shared vertex/index buffers before CPU Embree refits. Final refit policy admits BVH2 and CPU Embree, identity plain meshes and sunlight; motion, subdivision, transformed meshes, unsupported geometry/shaders and other backends retain static fallback. |
| `98ade547af4f` | General Blender / Cycles | Replace volume-mesh closure's multiset with directed-edge bookkeeping in a hash table. Retain duplicate-direction/non-manifold rejection. |
| `67930105c4ff` | StereoOcean fork / camera LOD | Select independent quadtree-root batches in parallel while preserving the previous leaf, vertex, face and attribute order. Changes scheduling, not the LOD policy or tolerance. |
| `eec8fbc0f84a` | StereoOcean fork / camera LOD | Count balancing splits, allocate the final leaf count once, and copy ordered ranges in parallel. Preserve child order and split decisions. |
| `8a65770d275f` | General Blender / Cycles | Reject provably open volume meshes using cancelling unsigned edge sums before allocating the edge table and copying positions. A zero sum still runs the exact closure check. |
| `4c0c626ae35a` | General Blender / Cycles | Count and pack BVH2 subtrees in parallel using preassigned ranges that preserve the original packed indices. Small trees retain serial handling. |
| `7040305c067b` | General Blender / Cycles, Ada CUDA | Use 192-thread shadow-shading blocks when SM 8.9's occupancy choice is 384 and the smaller blocks preserve potential resident threads. Other kernels/architectures retain existing selection. |
| `0a1015b64872` | General Blender / Cycles, Ada CUDA | Enable SVM jump-table generation for SM 8.9 with CUDA >= 12.3. Preserve the existing SM 12.0 policy. Performance qualified on len's RTX 4090. |
| `288a3701d5de` | General Blender / Cycles | Resolve the mandatory shader-entry jump before entering the SVM interpreter loop. Source change is shared across SVM devices; production speed/numerical qualification is on len SM 8.9 CUDA. |

There are no StereoOcean Python application changes in this native series.
Its companion StereoOcean commits contain profiling harnesses, numerical
checks and evidence. Qualification uses StereoOcean scenes; that workload
alone does not make a Cycles optimization application-specific.

## Numerical and build provenance

Geometry, depth and disparity checks passed in the recorded qualification.
Rendered RGB is judged against unchanged-build repeat variation, including
adaptive-sampling discontinuities; bitwise RGB equality is not guaranteed.
The latest SVM comparison initially mixed CUDA 12.9 and 13.4. Matched-toolkit
controls qualified the original source change; no diagnostic machine-code
math patch is part of this series.

The retained validated bundle is
`len:/home/xangma/cycles-instruction-20261112/final/blender`.
It combines the previously qualified host executable with a rebuilt SM 8.9
CUDA 12.9.86 kernel and shared runtime resources. Its `final/build.json`
records the source commit and compiler/kernel hashes. The reused executable
reports base hash `91bd7f03eddf`; this is not a complete release build at
`288a3701d5de`.

The separate shared Linux install checked on 17 November 2026 reports
`f8a2d7526d2a` (built 12 November). That branch contains the Ocean UI/control
update and excludes these 17 commits. Publishing this optimization branch
does not update that install or produce a release package.

Detailed reports are in the companion StereoOcean branch
`codex/ocean-reuse-20261110`, under `tools/`: `ocean_modifier_profile.rst`,
`ocean_cuda_profile.rst`, `ocean_reuse_profile.rst`,
`ocean_cycles_bvh_profile.rst`, `cycles_production_profile.rst`,
`ocean_lod_parallel_profile.rst`, `ocean_lod_balance_profile.rst`,
`cycles_volume_precheck_profile.rst`, `cycles_bvh_packing_profile.rst`,
`cycles_cuda_shadow_launch_profile.rst`, `cycles_cuda_dispatch_profile.rst`
and `cycles_cuda_entry_instruction_trace.rst`, with adjacent result JSONs.
Reported speedups have different incremental baselines; do not add them.
