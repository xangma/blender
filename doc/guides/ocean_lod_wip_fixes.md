# Ocean LOD WIP Fix List

This note tracks fixes needed before the foam/spray and repeated-tile WIP can be
treated as production or paper-baseline code.

## Foam and Spray Cycles Attributes

- Done: Replace per-pixel `BKE_ocean_eval_ij()` texture filling with a bulk extraction
  path. `BKE_ocean_foam_spray_data_get()` now fills one foam/spray texture under a
  single ocean read lock. A possible follow-up is sharing one cached pass when both foam
  and spray are requested.
- Done: Keep the benchmark from masking dense fallback. The supersample benchmark now
  requires camera LOD metadata and a real LOD geometry reduction before rendering.
- Verify SVM and OSL parity for attribute lookup. Foam/spray attribute nodes should
  sample the same full-spectrum texture on CPU/GPU, SVM/OSL, and motion-blur pre/post
  time samples.
- Decide whether bump-offset attribute paths need full-spectrum sampling too. If foam
  or spray is used through bump/displacement-style attribute offsets, `NODE_ATTR_BUMP_DX`
  and `NODE_ATTR_BUMP_DY` currently need a deliberate behavior.
- Preserve dense fallback outside supported render paths. Cached oceans, stereo dataset
  mode, non-Cycles render engines, and mesh apply/export flows should still fail closed
  to dense geometry where shader-side full-spectrum sampling is unavailable.

Acceptance checks:

- Foam-only and foam+spray Cycles renders stay adaptive and match dense reference within
  the existing RGB thresholds.
- Non-Cycles and cached cases still produce dense-equivalent topology.
- CPU and GPU image paths produce matching foam/spray attribute values.
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
- Partially done: Expand tests beyond `repeat_x=2, repeat_y=1`. Current smoke coverage
  exercises `repeat_x=2, repeat_y=2`. Still cover non-square repeat counts, `size != 1`,
  high LOD level requests that do not divide both axes evenly, and visible footprints
  crossing tile boundaries.
- Validate quadtree roots and balancing on rectangular domains. `dense_cells_x` and
  `dense_cells_y` can differ, so root selection, neighbor maps, and 2:1 balancing must
  not assume a square dense cell grid.
- Confirm reference sampling and coverage reports use repeated-domain UVs consistently.
  Dense reference samples should compare against the same physical repeated tile that the
  LOD vertex represents, while shader texture lookups remain periodic.

Acceptance checks:

- Visible dense samples in repeated tiles are covered by LOD leaves with no gaps.
- Repeated-domain LOD remains adaptive instead of falling back to dense geometry.
- Geometry, reprojection, depth, normal, and RGB thresholds pass for repeat X, repeat Y,
  and mixed repeat scenarios.
- Boundary samples at tile seams match the dense reference without half-cell shifts or
  off-by-one coverage holes.
