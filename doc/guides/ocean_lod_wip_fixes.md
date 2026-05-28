# Ocean LOD WIP Fix List

This note tracks fixes needed before the foam/spray and repeated-tile WIP can be
treated as production or paper-baseline code.

## LOD Compatibility Checklist

Use this as the running checklist for validating that Camera LOD behaves like the
dense Ocean modifier where it should, and falls back deliberately where it cannot.

Fallback paths:

- [x] Cached ocean uses dense-equivalent topology or an explicit dense fallback.
- [x] Non-Cycles foam/spray evaluation uses an explicit dense fallback.
- [x] Non-Cycles final-render geometry-only fallback is covered by tests.
- [x] Apply modifier and mesh conversion produce dense-equivalent meshes.
- [x] OBJ, PLY, and STL export paths produce dense-equivalent meshes.

Repeat-domain coverage:

- [x] Curated `repeat_x=2`, `repeat_y=2` dense-vs-LOD benchmark passes.
- [x] Spot checks pass for `3x1`, `1x3`, and `3x2` repeated domains.
- [x] `size != 1` repeated domains pass visible-coverage checks.
- [x] Camera footprints crossing repeated tile seams pass without coverage gaps.
- [x] High requested LOD levels that do not divide both axes cleanly are covered by tests.

Camera and view modes:

- [x] Stereo dataset scenario is present in the curated benchmark suite.
- [x] Stereo/multiview dense-vs-LOD render comparisons pass.
- [x] Animated camera dense-vs-LOD comparisons pass.
- [x] Wide-FOV, grazing, very close, and near-plane clipping cases pass.

Motion:

- [x] Ocean time motion blur preserves foam/spray dense parity.
- [x] Object transform motion blur passes dense-vs-LOD checks.
- [x] Camera motion blur passes dense-vs-LOD checks.
- [x] Topology remains stable or falls back deliberately when motion samples differ.

Shader and attribute usage:

- [x] Foam and spray Attribute `Color` output matches dense on CPU SVM.
- [x] Foam and spray Attribute `Color` output matches dense on CPU OSL.
- [x] Foam and spray Attribute `Color` output matches dense on Metal and Metal-RT.
- [x] Foam and spray Attribute `Fac` output matches dense.
- [x] Bump-offset attribute lookups have a documented and tested behavior.
- [x] Multiple foam/spray attribute users pass dense-vs-LOD checks.
- [x] Multiple Ocean materials in one scene pass dense-vs-LOD checks.

Geometry and shading quality:

- [x] High-energy dense-ceiling and grazing-light scenarios are in the curated suite.
- [x] High choppiness and high wind dense-vs-LOD checks pass.
- [x] Full-spectrum radius behavior is covered.
- [x] LOD transitions and 2:1 balancing are checked visually and numerically.

Performance and scale:

- [x] Foam attribute CPU and CPU-OSL render timings show no measurable slowdown in the
  current 64 px comparison case.
- [x] Large ocean resolutions have bounded foam/spray texture prep time and memory.
- [x] CPU and GPU render timings are measured for representative dense and LOD scenes.
- [x] Multiple Ocean objects in one scene are covered.

Device coverage:

- [x] CPU SVM, CPU OSL, Metal, and Metal-RT smoke/parity coverage passes.
- [x] CUDA, OptiX, HIP, and oneAPI are checked where hardware is available.

## Foam and Spray Cycles Attributes

- Done: Replace per-pixel `BKE_ocean_eval_ij()` texture filling with a bulk extraction
  path. `BKE_ocean_foam_spray_data_get()` now fills one foam/spray texture under a
  single ocean read lock. A possible follow-up is sharing one cached pass when both foam
  and spray are requested.
- Done: Keep the benchmark from masking dense fallback. The supersample benchmark now
  requires camera LOD metadata and a real LOD geometry reduction before rendering.
- Done: Add dense-vs-LOD foam parity coverage. Foam can now be rendered as a pure
  emission attribute through the shared metrics helpers, with both a curated benchmark
  scenario and a physics smoke test covering foam-only and foam+spray modifier setups.
- Done: Match shader-side foam/spray attribute sampling to dense byte-color semantics,
  including scene-linear byte-color conversion and dense quad triangle interpolation.
- Done: Fix dense spray corner writes. The spray writer now advances per corner instead
  of leaving most corners at the default byte-color value.
- Done: Match spray's legacy byte-channel conversion. Shader-side spray texture data now
  follows dense byte-color truncation/wrapping before Cycles converts to scene-linear.
- Done: Match dense motion-blur behavior. Foam/spray shader-side attributes stay on the
  center frame under motion blur because dense Cycles color attributes do the same.
- Done: Verify SVM, OSL, Metal, and Metal-RT parity for foam/spray attribute lookup.
- Done: Document and test bump-offset attribute behavior. Foam/spray bump-height lookups
  currently go through Cycles' mesh attribute differential path and match dense within
  the render thresholds.
- Done: Preserve dense fallback outside supported render paths. Cached oceans,
  non-Cycles render engines, and mesh apply/export flows fail closed to dense geometry
  where shader-side full-spectrum sampling is unavailable.

Acceptance checks:

- Foam-only and foam+spray Cycles renders stay adaptive and match dense reference within
  the existing RGB thresholds.
- Non-Cycles and cached cases still produce dense-equivalent topology.
- CPU, OSL, Metal, and Metal-RT image paths produce matching foam/spray attribute values.
- Texture preparation time is bounded for high ocean resolutions and does not scale with
  one mutex lock per texel.

## Repeated Tiles

- Done: Audit repeated-domain math against dense generated geometry for the current WIP
  smoke coverage. The camera LOD domain now matches the dense grid from
  `[-0.5 * domain_size, -0.5 * domain_size]` to `domain_min + repeat * domain_size`
  for both X and Y in the exercised case.
- Done: Update camera-anchor and validation helpers for repeated domains in the Python
  coverage test. The camera anchor now clamps against repeated-domain min/max instead
  of the base tile.
- Done: Expand tests beyond `repeat_x=2`, `repeat_y=1`. Current smoke and benchmark
  coverage exercises square, non-square, scaled, seam-crossing, and awkward
  non-divisible repeated domains.
- Done: Keep the repeat-tile benchmark strict while tolerating off-camera LOD vertex
  position error. Visible reprojection, depth, normal, and RGB thresholds remain tight.
- Done: Validate quadtree roots and balancing on rectangular domains. The mixed
  repeat-domain smoke tests cover cases where `dense_cells_x` and `dense_cells_y` differ
  and assert the face-domain 2:1 split-level invariant.
- Done: Confirm reference sampling and coverage reports use repeated-domain UVs
  consistently. Dense reference samples compare against the same physical repeated tile
  represented by the LOD vertex, while shader texture lookups remain periodic.

Acceptance checks:

- Visible dense samples in repeated tiles are covered by LOD leaves with no gaps.
- Repeated-domain LOD remains adaptive instead of falling back to dense geometry.
- Geometry, reprojection, depth, normal, and RGB thresholds pass for repeat X, repeat Y,
  and mixed repeat scenarios.
- Boundary samples at tile seams match the dense reference without half-cell shifts or
  off-by-one coverage holes.

## Camera and View Modes

- Done: Add stereo/multiview RGB parity coverage. The smoke test renders both stereo eyes
  and compares the materialized LOD output against dense geometry.
- Done: Add animated-camera parity coverage. The smoke test validates keyed camera
  transforms at frame 1 and frame 12, then renders the final keyed frame against dense.
- Done: Add extreme view coverage for wide FOV, grazing camera angle, and a very close
  near-clipped camera. These remain adaptive and pass the dense reference geometry
  thresholds.

## Motion

- Done: Add object-transform motion-blur parity coverage. The test moves the Ocean object
  across centered shutter samples, verifies the endpoint LOD topology changes, and
  compares the center-frame motion-blur render against dense geometry.
- Done: Add camera motion-blur parity coverage. The test moves the camera across centered
  shutter samples, verifies the endpoint LOD topology changes, and compares the
  motion-blur render against dense geometry.

## Shader and Attribute Usage

- Done: Add CPU SVM parity coverage for foam and spray Attribute `Fac` output. This uses
  the existing full-spectrum shader-side attribute path and matches dense within the
  foam/spray RGB thresholds.
- Done: Add bump-height parity coverage for foam and spray Attribute `Fac`. The test
  documents the current behavior: bump-offset lookups use Cycles' mesh attribute
  differential path, and the LOD render remains within the dense reference thresholds.
- Done: Add multiple-user and multi-material scene coverage. One material combines foam
  and spray Attribute nodes, and a second scene renders two Ocean objects with different
  attribute materials while toggling both objects between LOD and dense.

## Geometry and Shading Quality

- Done: Add high-choppiness/high-wind strict validation. The smoke test keeps adaptive
  geometry, stays near the dense ceiling, checks topology balance, and compares a Cycles
  render against dense geometry.
- Done: Add full-spectrum radius coverage. The test compares default and expanded
  `lod_camera_full_spectrum_radius` settings and asserts that the expanded radius keeps
  more split-level-0 vertices and reduces less geometry near the camera anchor.
- Done: Add LOD transition coverage. The test exercises multiple split levels in a
  general-render scene, checks the face-domain 2:1 balancing metadata, and runs an RGB
  dense-vs-LOD render comparison.

## Performance and Scale

- Done: Add a larger foam/spray profile case. The smoke test verifies reduced LOD
  geometry, bounds foam/spray prep time against dense, and checks that Cycles split
  resource sync remains below dense triangle/vertex counts.
- Done: Add representative render timing coverage. CPU timings are asserted on every
  run, and the first available non-RT GPU device is measured when available.
- Done: Extend the existing multi-material scene check to assert that two Ocean objects
  render in one dense-vs-LOD comparison and record separate LOD and dense timings.

## Device Coverage

- Done: CPU SVM, CPU OSL, Metal, and Metal-RT pass the local smoke/parity coverage.
- Done: CUDA dense-vs-LOD render parity was checked on `len` with the existing Linux
  CUDA build. The host reported available Cycles devices `CPU` and `CUDA`; OptiX, HIP,
  and oneAPI were not available on that build/host.
