# Ocean Multi-Rig Benchmark

Date: 2026-05-06

This report benchmarks the StereoOcean `configs/previews/multi_rig.json` settings against three
ocean modifier paths:

- Dense baseline: Blender at `b8175a8c27cd`, dense path plus the earlier ocean bugfixes.
- Dense optimized: clean `lod-changes` build at `fe78134f3665`, forced through `ocean_mode=dense_reference`.
- LOD optimized: clean `lod-changes` build at `fe78134f3665`, using `ocean_mode=camera_lod`.

The raw `multi_rig.json` config has no seed, so the benchmark adds `seed=42` and
`random_seed=42`.

## Executive Summary

Primary repeat artifact:
`/tmp/ocean_multi_rig_benchmark_seed42_clean_fe781_repeat3_20260506_092226`

| Variant | Runs | Full process wall | Render operation wall | Peak RSS | Result vs dense baseline |
| --- | ---: | ---: | ---: | ---: | --- |
| Dense baseline | 3 | 40.54 s mean | 17.24 s mean | 14.42 GB mean | Reference |
| Dense optimized | 3 | 29.19 s mean | 14.86 s mean | 14.60 GB mean | Dense mesh matches baseline in sampled probe |
| LOD optimized | 3 | 16.71 s mean | 10.16 s mean | 11.51 GB mean | Faster, with measured image divergence |

| Comparison | Full wall delta | Full wall speedup | Render wall delta | Peak RSS delta |
| --- | ---: | ---: | ---: | ---: |
| Dense optimized vs dense baseline | -11.35 s | 1.39x | -2.38 s | +0.18 GB |
| LOD optimized vs dense baseline | -23.83 s | 2.43x | -7.08 s | -2.91 GB |
| LOD optimized vs dense optimized | -12.48 s | 1.75x | -4.70 s | -3.09 GB |

At this production-style `960x540`, 256-sample stereo setting, the final LOD path is faster than
dense across full process wall, render operation wall, and peak resident memory. The main dense
win arrived in `a3aace2be24`; the LOD path then improved from 24.19 s at initial implementation to
16.90 s in the per-commit sweep.

## Test Setup

| Item | Value |
| --- | --- |
| Remote host | `roni1` |
| GPUs | 2x NVIDIA GeForce RTX 3090, CUDA, driver `590.48.01` |
| StereoOcean config | `/home/xangma/repos/stereoocean/configs/previews/multi_rig.json` |
| Deterministic override | `seed=42`, `random_seed=42` |
| Rig | `h30_pitch32`, height `30.0`, pitch `32.0`, focal length `18.0` |
| Render | `960x540`, 256 Cycles samples, GPU, CUDA |
| Frame range | frame `1` only |
| Ocean | extent `1000.0`, wave scale `1.4`, choppiness `1.45`, wind velocity `9.0`, resolution `64` |
| Dense baseline build | `/home/xangma/repos/blender-git/build_linux_dense_baseline_cuda/bin/blender` |
| Clean optimized build | `/home/xangma/repos/blender-git/build_linux_lod_clean_fe781_cuda/bin/blender` |
| Clean optimized source | `/home/xangma/repos/blender-git/blender_lod_clean_fe781` |
| Profile env | `BLENDER_OCEAN_CAMERA_LOD_PROFILE=1` |

Exact source state:

| Source | State |
| --- | --- |
| Dense baseline binary | `b8175a8c27cd`, clean baseline source |
| Clean optimized binary | build hash `fe78134f3665`, branch `lod-clean-fe781`, clean source |
| Local change references | committed local branch through `fe78134f3665` plus report/probe tooling in this working tree |
| StereoOcean | `27f75826aae0`, dirty tree during the runs |

The clean optimized build was configured for CUDA benchmarking and disabled irrelevant OneAPI
binary generation to avoid long AOT compile time. Runtime render device was CUDA.

## Timing Definitions

| Metric | Source | Meaning |
| --- | --- | --- |
| Full process wall | `/usr/bin/time -v` | Blender startup, StereoOcean setup, render, image writes, shutdown |
| Generator frame wall | `metadata.json` | StereoOcean frame timer for component updates plus render call |
| Render operation wall | Blender render log | Blender's `Time: ... (Saving: ...)` for both stereo cameras |
| Ocean component update | `metadata.json` | StereoOcean ocean-system update before render |
| Modifier profile | `BLENDER_OCEAN_CAMERA_LOD_PROFILE=1` | Ocean modifier stages from optimized Blender builds |

For LOD, `Ocean component update` is near zero because the meaningful ocean work is triggered by
render depsgraph evaluation. Use the modifier profile for LOD ocean cost.

## Repeat Timing

| Variant | Runs | Mode | Full process wall | Generator frame wall | Render operation wall | Ocean component | Peak RSS |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| Dense baseline | 3 | `dense_reference` | 40.54 (min 40.46, max 40.64, sd 0.09) | 39.40 (min 39.31, max 39.48, sd 0.08) | 17.24 (min 17.15, max 17.35, sd 0.10) | 9.20 (min 9.13, max 9.26, sd 0.07) | 14.42 (min 14.39, max 14.47, sd 0.04) |
| Dense optimized | 3 | `dense_reference` | 29.19 (min 28.66, max 30.07, sd 0.77) | 27.75 (min 27.61, max 27.88, sd 0.14) | 14.86 (min 14.81, max 14.92, sd 0.06) | 7.09 (min 7.05, max 7.13, sd 0.04) | 14.60 (min 14.48, max 14.73, sd 0.13) |
| LOD optimized | 3 | `camera_lod` | 16.71 (min 16.69, max 16.73, sd 0.02) | 15.67 (min 15.65, max 15.69, sd 0.02) | 10.16 (min 10.13, max 10.18, sd 0.03) | 0.00 (min 0.00, max 0.00, sd 0.00) | 11.51 (min 11.49, max 11.53, sd 0.02) |

Per-run timing:

| Variant | Run | Full wall | Frame wall | Render wall | Peak RSS | Modifier total |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Dense baseline | 1 | 40.46 | 39.31 | 17.15 | 14.39 GB | n/a |
| Dense baseline | 2 | 40.53 | 39.42 | 17.21 | 14.40 GB | n/a |
| Dense baseline | 3 | 40.64 | 39.48 | 17.35 | 14.47 GB | n/a |
| Dense optimized | 1 | 30.07 | 27.88 | 14.92 | 14.73 GB | 9.63 |
| Dense optimized | 2 | 28.84 | 27.77 | 14.84 | 14.48 GB | 9.61 |
| Dense optimized | 3 | 28.66 | 27.61 | 14.81 | 14.60 GB | 9.53 |
| LOD optimized | 1 | 16.69 | 15.65 | 10.17 | 11.49 GB | 2.80 |
| LOD optimized | 2 | 16.73 | 15.69 | 10.18 | 11.51 GB | 2.80 |
| LOD optimized | 3 | 16.72 | 15.67 | 10.13 | 11.53 GB | 2.80 |

LOD mesh reduction in this seeded multi-rig case:

| Metric | Dense optimized | LOD optimized | Reduction vs dense |
| --- | ---: | ---: | ---: |
| Vertices | 16,785,409 | 6,714,523 | 60.00% |
| Faces | 16,777,216 | 6,706,438 | 60.03% |

## Per-Commit Timing

Artifact:
`/tmp/ocean_multi_rig_per_commit_seed42_clean_fe781_20260506_093539`

The baseline dense reference in this sweep was one seeded run at 40.48 s full wall, 17.15 s render
wall, and 14.50 GB peak RSS. Each runtime commit below was built cleanly and run once for dense and
LOD. Commits `a52042486ff` and `c677031e0d3` are omitted from the timing table because they only add
benchmark helpers/docs and do not change runtime behavior.

| Commit | Change | Dense full | Dense render | LOD full | LOD render | LOD verts | LOD RGB mean / p95 / max vs baseline | Dense RGB mean / p95 / max vs baseline |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| `e65397f1044` | camera LOD initial | 40.88 | 17.26 | 24.19 | 10.60 | 6,714,523 | 0.00007487 / 0.000000 / 0.076471 | 0.00000011 / 0.000000 / 0.001307 |
| `a3aace2be24` | eval paths, dense and LOD | 28.55 | 14.75 | 18.80 | 10.63 | 6,714,523 | 0.00006529 / 0.000000 / 0.076471 | 0.00000012 / 0.000000 / 0.001307 |
| `86d7daa5fd8` | topology and split sim | 28.59 | 14.82 | 17.60 | 10.41 | 6,714,523 | 0.00006528 / 0.000000 / 0.076471 | 0.00000883 / 0.000000 / 0.010458 |
| `3691b0837cf` | point-domain custom normals | 28.42 | 14.76 | 17.79 | 10.62 | 6,714,523 | 0.00006527 / 0.000000 / 0.076471 | 0.00000013 / 0.000000 / 0.001307 |
| `003d71689f4` | split-normal parallel work | 28.54 | 14.81 | 16.98 | 10.25 | 6,714,523 | 0.00007190 / 0.000000 / 0.075817 | 0.00000013 / 0.000000 / 0.001307 |
| `c1d8b6ba4b5` | leaf-validation short circuit | 28.55 | 14.84 | 16.90 | 10.24 | 6,714,523 | 0.00006528 / 0.000000 / 0.076471 | 0.00000937 / 0.000000 / 0.011765 |
| `3fad6db6fa7` | dense-ceiling route | 28.65 | 14.81 | 17.05 | 10.31 | 6,714,523 | 0.00006528 / 0.000000 / 0.076471 | 0.00000014 / 0.000000 / 0.001307 |
| `fe78134f3665` | split moment accumulation | 28.76 | 14.80 | 16.90 | 10.33 | 6,714,523 | 0.00007146 / 0.000000 / 0.076471 | 0.00000012 / 0.000000 / 0.001307 |

Modifier profile from the same per-commit sweep:

| Commit | Dense modifier total | LOD modifier total | LOD faces | Build status |
| --- | ---: | ---: | ---: | --- |
| `e65397f1044` | 22.01 | 9.37 | 6,706,438 | 0 |
| `a3aace2be24` | 9.60 | 3.96 | 6,706,438 | 0 |
| `86d7daa5fd8` | 9.58 | 3.13 | 6,706,438 | 0 |
| `3691b0837cf` | 9.56 | 3.10 | 6,706,438 | 0 |
| `003d71689f4` | 9.57 | 2.89 | 6,706,438 | 0 |
| `c1d8b6ba4b5` | 9.65 | 2.83 | 6,706,438 | 0 |
| `3fad6db6fa7` | 9.64 | 2.87 | 6,706,438 | 0 |
| `fe78134f3665` | 9.59 | 2.79 | 6,706,438 | 0 |

## Ocean Modifier Profile

The dense baseline build predates the profile logging. Values below are repeat means from the clean
`fe78134f3665` build.

| Variant | Modifier evals | Modifier total | Simulation | Geometry generate | Settings / leaf build | Displacement |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Dense optimized | 5 per run | 9.59 s total, 1.92 s mean | 2.37 s total | 6.73 s total | 0.00 s | 0.36 s total |
| LOD optimized | 1 per run | 2.80 s total | 0.85 s | 0.81 s | 1.02 s | 0.10 s |

## Correctness Checks

### PNG Render Difference

These are dense-baseline image comparisons for both left and right cameras, averaged across three
runs. Metrics are absolute differences in the 8-bit PNG render outputs.

| Candidate | RGB mean / p95 / max | Luminance mean / p95 / max | Luminance-gradient mean / p95 / max |
| --- | --- | --- | --- |
| Dense optimized | 0.00000012 / 0.000000 / 0.001307 | 0.00000010 / 0.000000 / 0.002805 | 0.00000012 / 0.000000 / 0.001402 |
| LOD optimized | 0.00006527 / 0.000000 / 0.076471 | 0.00006658 / 0.000000 / 0.081354 | 0.00005747 / 0.000109 / 0.038394 |

Per-camera repeat means:

| Candidate | Camera | RGB mean / p95 / max | Luminance mean / p95 / max | Luminance-gradient mean / p95 / max |
| --- | --- | --- | --- | --- |
| Dense optimized | Left | 0.00000012 / 0.000000 / 0.001307 | 0.00000009 / 0.000000 / 0.002805 | 0.00000011 / 0.000000 / 0.001402 |
| Dense optimized | Right | 0.00000012 / 0.000000 / 0.001307 | 0.00000010 / 0.000000 / 0.002805 | 0.00000012 / 0.000000 / 0.001402 |
| LOD optimized | Left | 0.00005388 / 0.000000 / 0.064052 | 0.00005509 / 0.000000 / 0.067202 | 0.00005041 / 0.000101 / 0.031583 |
| LOD optimized | Right | 0.00007667 / 0.000000 / 0.088889 | 0.00007808 / 0.000000 / 0.095507 | 0.00006452 / 0.000117 / 0.045204 |

Diff images and per-run JSONs live under:

`/tmp/ocean_multi_rig_benchmark_seed42_clean_fe781_repeat3_20260506_092226/comparisons/run_*`

### EXR Float Difference

Artifact:
`/tmp/ocean_multi_rig_exr_seed42_clean_fe781_20260506_092822`

This is a single-run 32-bit OpenEXR check using `oiiotool --diff`, because Blender's Python image
loader returned zero-sized buffers for these EXR files in the existing render comparison helper.
The EXR files are valid `960 x 540`, 3-channel, float OpenEXR images according to `oiiotool --info`.

| Candidate | Camera | Mean error | RMS error | Max error | Pixels over 1e-6 |
| --- | --- | ---: | ---: | ---: | ---: |
| Dense optimized | Left | 0.00000382 | 0.00004752 | 0.011563 | 28.6% |
| Dense optimized | Right | 0.00000315 | 0.00003931 | 0.005938 | 27.4% |
| LOD optimized | Left | 0.00006717 | 0.00110929 | 0.104058 | 62.7% |
| LOD optimized | Right | 0.00007099 | 0.00108020 | 0.121554 | 63.1% |

| Candidate | Mean error avg | RMS error avg | Max error avg | Pixels over 1e-6 avg |
| --- | ---: | ---: | ---: | ---: |
| Dense optimized | 0.00000348 | 0.00004342 | 0.008750 | 28.0% |
| LOD optimized | 0.00006908 | 0.00109474 | 0.112806 | 62.9% |

### Dense Mesh Probe

Artifact:
`/tmp/ocean_multi_rig_mesh_probe_seed42_clean_fe781_20260506_093317`

The dense mesh probe evaluates the final dense optimized path and the dense baseline path with
`--dry-run`, then compares 65,536 deterministic sampled vertices/normals from the evaluated Ocean
mesh.

| Metric | Result |
| --- | --- |
| Reference build | `b8175a8c27cd` |
| Candidate build | `fe78134f3665` |
| Topology | matches: 16,785,409 verts, 16,777,216 polys |
| Position hash | matches |
| Normal hash | matches |
| Position distance mean / p95 / max | 0.0 / 0.0 / 0.0 |
| Position component abs mean / p95 / max | 0.0 / 0.0 / 0.0 |
| Normal distance mean / p95 / max | 0.0 / 0.0 / 0.0 |
| Normal angle mean / p95 / max | 0.000000215 deg / 0.000001207 deg / 0.000001207 deg |
| Point attributes | no reference-only or candidate-only sampled point attributes |

This supports the dense-path requirement: the final dense optimized mesh is identical to dense
baseline in the sampled topology, position, and normal probe. The tiny dense render differences are
therefore below mesh/attribute level in this probe and are likely render pipeline residue.

## Change Breakdown

| Change | Commit | Code location | Dense affected? | Notes |
| --- | --- | --- | --- | --- |
| Dense baseline normal-index bugfix | `b8175a8c27cd` | `source/blender/blenkernel/intern/ocean.cc:1851` | Yes, baseline fix | Corrects the normal-z FFT multiplier index to use `kz[j]`. |
| Camera-anchored LOD implementation | `e65397f1044` | `source/blender/modifiers/intern/MOD_ocean.cc`, `source/blender/blenkernel/intern/ocean.cc`, Cycles ocean closure files | Mostly LOD | Adds camera LOD mesh path, split spectrum runtime, Cycles integration, RNA, tests, and docs. |
| Dense scoped read lock and parallel displacement | `a3aace2be24` | `source/blender/modifiers/intern/MOD_ocean.cc:4526`, `source/blender/modifiers/intern/MOD_ocean.cc:4702` | Yes | Main dense-path speedup. Dense optimized must remain render-equivalent to dense baseline. |
| LOD breadth-frontier leaf validation | `a3aace2be24` | `source/blender/modifiers/intern/MOD_ocean.cc:3245` | LOD only | Parallelizes independent leaf split decisions. |
| LOD leaf balancing parallelization | `a3aace2be24` | `source/blender/modifiers/intern/MOD_ocean.cc:3562`, `source/blender/modifiers/intern/MOD_ocean.cc:3647` | LOD only | Parallel owner-cell map fill and neighbor checks. |
| Benchmark and comparison helpers | `a52042486ff` | `tests/python/ocean_camera_lod_benchmark.py`, `tests/python/ocean_camera_lod_compare_renders.py`, `tests/python/modules/ocean_camera_lod_metrics.py` | No runtime change | Adds dense baseline, dense optimized, LOD optimized comparison tooling. |
| LOD topology two-pass assembly | `86d7daa5fd8` | `source/blender/modifiers/intern/MOD_ocean.cc:3739`, `source/blender/modifiers/intern/MOD_ocean.cc:3777` | LOD only | Builds adaptive topology in deterministic passes with parallel per-leaf corner generation and direct result mesh writes. |
| Level-parallel split pyramid | `86d7daa5fd8` | `source/blender/blenkernel/intern/ocean.cc:692`, `source/blender/blenkernel/intern/ocean.cc:725` | LOD only | Builds independent reduced split levels in parallel. |
| LOD point-domain custom normals | `3691b0837cf` | `source/blender/modifiers/intern/MOD_ocean.cc:4810` | LOD only | Stores generated LOD normals as point-domain `custom_normal` instead of corner custom-normal conversion. |
| Split normal computation parallelization | `003d71689f4` | `source/blender/blenkernel/intern/ocean.cc:497`, `source/blender/blenkernel/intern/ocean.cc:634` | LOD split runtime | Parallelizes expensive split-level derivative and normal work while keeping deterministic moment behavior. |
| Leaf hard-cap early exit | `c1d8b6ba4b5` | `source/blender/modifiers/intern/MOD_ocean.cc:2662`, `source/blender/modifiers/intern/MOD_ocean.cc:2773` | LOD only | Stops validation sampling once the hard error cap has already failed. |
| Dense-ceiling early completion and dense route | `3fad6db6fa7` | `source/blender/modifiers/intern/MOD_ocean.cc:2264`, `source/blender/modifiers/intern/MOD_ocean.cc:3276` | LOD dense-ceiling cases | Routes full dense-ceiling LOD cases through the dense mesh path instead of emitting every finest-grid leaf. |
| Split moment accumulation and base write-back cleanup | `fe78134f3665` | `source/blender/blenkernel/intern/ocean.cc:602`, `source/blender/blenkernel/intern/ocean.cc:617` | LOD split runtime | Parallelizes moment accumulation in deterministic row chunks and removes redundant base-level write-back. |
| Repeat artifact report generator | working tree | `tests/python/ocean_multi_rig_benchmark_report.py` | No runtime change | Parses multi-rig benchmark artifacts into Markdown and JSON summaries. |
| Per-commit artifact report generator | working tree | `tests/python/ocean_multi_rig_per_commit_report.py` | No runtime change | Parses the per-runtime-commit benchmark sweep into Markdown and JSON summaries. |
| Mesh probe and comparison helpers | working tree | `tests/python/ocean_multi_rig_mesh_probe.py`, `tests/python/ocean_multi_rig_mesh_compare.py` | No runtime change | Compares evaluated dense meshes/attributes without rendering. |
| EXR comparison helper | working tree | `tests/python/ocean_multi_rig_exr_compare.py` | No runtime change | Parses `oiiotool --diff` output for float render comparison. |
| Render compare JSON-only mode | working tree | `tests/python/ocean_camera_lod_compare_renders.py:25` | No runtime change | Adds `--no-diff-images` for metric-only comparisons. |

## Reproducibility

Config generation used the base `multi_rig.json` with these changes:

```bash
jq --arg output "$out" --arg mode "$mode" \
  '.output = $output
   | .ocean_mode = $mode
   | .seed = 42
   | .random_seed = 42
   | .file_format = "PNG"
   | .color_depth = "8"
   | .color_mode = "RGB"
   | if $mode == "camera_lod" then .camera_lod_levels = "auto" else del(.camera_lod_levels) end' \
  /home/xangma/repos/stereoocean/configs/previews/multi_rig.json > "$case_dir/config.json"
```

Each variant run used this command shape:

```bash
cd /home/xangma/repos/stereoocean
BLENDER_OCEAN_CAMERA_LOD_PROFILE=1 \
  /usr/bin/time -v -o "$case_dir/time.txt" \
  "$blender" -b -P scripts/generate_ocean.py -- --config "$case_dir/config.json" \
  > "$case_dir/blender.log" 2>&1
```

The repeat report can be regenerated locally from copied artifacts:

```bash
python3 tests/python/ocean_multi_rig_benchmark_report.py \
  --artifact-root /tmp/ocean_multi_rig_benchmark_seed42_clean_fe781_repeat3_20260506_092226_local \
  --out-md /tmp/ocean_multi_rig_clean_repeat3_summary.md \
  --out-json /tmp/ocean_multi_rig_clean_repeat3_summary.json
```

The per-commit report can be regenerated locally from copied artifacts:

```bash
python3 tests/python/ocean_multi_rig_per_commit_report.py \
  --artifact-root /tmp/ocean_multi_rig_per_commit_seed42_clean_fe781_20260506_093539_local \
  --out-md /tmp/ocean_multi_rig_per_commit_summary.md \
  --out-json /tmp/ocean_multi_rig_per_commit_summary.json
```

The dense mesh probe used:

```bash
"$blender" -b --factory-startup --python tests/python/ocean_multi_rig_mesh_probe.py -- \
  --stereoocean-root /home/xangma/repos/stereoocean \
  --config "$config" \
  --out "$probe_json" \
  --sample-count 65536
```

The EXR float comparison used:

```bash
python3 tests/python/ocean_multi_rig_exr_compare.py \
  --reference "$dense_baseline_exr" \
  --candidate "$candidate_exr" \
  --out-json "$comparison_json"
```

## Limitations

- Final variant timings have n=3 repeats; per-commit timing has n=1 per commit.
- This is one scene, one seed, one rig, and one frame.
- The dense baseline build lacks modifier profile logging.
- PNG comparisons are 8-bit and include quantization residue.
- EXR comparisons are single-run `oiiotool --diff` metrics, not repeated statistics.
- Raw `multi_rig.json` remains non-deterministic unless a seed is injected.
- StereoOcean was dirty during the runs.
- Dense render diffs are tiny but not always bit-zero; the final dense mesh probe is exact for the
  sampled evaluated mesh.
