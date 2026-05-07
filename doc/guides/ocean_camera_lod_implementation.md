# Ocean Camera LOD Implementation

This document describes the current implementation of the Ocean modifier Camera LOD path. It is
based on the code in:

- `source/blender/modifiers/intern/MOD_ocean.cc`
- `source/blender/blenkernel/intern/ocean.cc`
- `source/blender/blenkernel/BKE_ocean.h`
- `intern/cycles/blender/mesh.cpp`
- `intern/cycles/scene/mesh.h`
- `intern/cycles/scene/object.cpp`
- `intern/cycles/scene/attribute.cpp`
- `intern/cycles/kernel/types.h`
- `intern/cycles/kernel/closure/bsdf_ocean.h`
- `intern/cycles/kernel/osl/closures_setup.h`

It is an implementation guide, not a design proposal.

## Code And History References

This guide was checked against the local worktree at commit `fe78134f366`. Code references use
file paths and function names instead of line numbers because this area is still moving quickly.

Useful local history checkpoints:

- `8fbfc6780a0` - Add support for split spectrum in ocean modifier
- `3526c1ef924` - Ocean: add stereo-faithful camera LOD and stereo API
- `e65397f1044` - Implement camera anchored ocean LOD
- `d9e69224bb2` - Skip redundant ocean LOD custom normals in Cycles
- `86d7daa5fd8` - Optimize ocean LOD topology and split simulation
- `3fad6db6fa7` - Route ocean LOD dense ceilings through dense path
- `fe78134f366` - Parallelize ocean split moment accumulation

## Relationship To Existing Notes

The repository already contains several Ocean Camera LOD notes, but none of them is an exact guide
to the current code.

- `doc/guides/ocean_camera_lod_progress.md` is a progress log. It contains useful history, but some
  entries describe older centered clipmap/ring implementations that are no longer the current
  method.
- `doc/guides/ocean_camera_lod_observable_error_design.md` describes the observable-error model
  that motivated the current allocator. It is still useful conceptually, but it is not an exact
  walkthrough of the adaptive leaf topology and current Cycles shading behavior.
- `doc/guides/ocean_camera_lod_parallelisation_options.md` is a benchmark and optimization log.

This file is intended to be the exact current-method reference.

## Entry Conditions And Fallbacks

Camera LOD is requested when `MOD_OCEAN_USE_CAMERA_LOD` is set and the Ocean modifier is in
`MOD_OCEAN_GEOM_GENERATE` mode.

Even when requested, the modifier falls back to dense geometry for these cases:

- baked/cache mode, because Camera LOD needs live full-spectrum simulation data
- foam or spray generation, because full-spectrum foam/spray sampling is not implemented for LOD
- applying the modifier to the original object, because that would bake the reduced surface
- final render in a non-Cycles engine, because render-equivalent residual shading is only provided
  for Cycles

Viewport and non-Cycles evaluated meshes can still use the LOD topology; they receive explicit
geometry-band displacement and a point-domain `custom_normal` fallback.

## Coordinate Spaces

The adaptive mesh is built in the modifier object's local ocean plane.

- Mesh reference coordinates use local `x/y` as the ocean plane.
- Ocean simulation coordinates use canonical ocean `x/z`, with ocean `y` as height.
- During displacement, ocean vertical displacement `disp_y` is added to mesh `z`.
- Choppy displacement `disp_x/disp_z` is added to mesh `x/y`.
- `ocean_geometry_normal` is stored in canonical ocean coordinates. Cycles maps it back to render
  space as needed.

The observable error tests compare candidate and reference positions in object-local units, then
project those positions through per-view object-to-camera matrices for pixel/depth checks.

## Split-Spectrum Runtime

When Camera LOD is enabled, the Ocean simulation builds a split runtime pyramid in `ocean.cc`. This
runtime is owned by `Ocean::_split_levels` and is rebuilt after the regular ocean simulation writes
the current `_htilda`, displacement, chop, and normal buffers.

The split runtime level count is not the same as the modifier's `lod_levels` UI value.

- The simulation grid is initialized as `resolution * resolution` samples per side.
- `ocean_split_level_count(size_x, size_y)` starts at level count `1`, then repeatedly halves both
  dimensions with integer division until both dimensions reach `1`.
- For square grids this is `floor(log2(resolution * resolution)) + 1`.
- Each level size is `max(M >> level, 1) x max(N >> level, 1)`.
- Example: modifier resolution `64` creates a `4096 x 4096` ocean grid and `13` split levels:
  `4096, 2048, 1024, ..., 2, 1`.
- Example: modifier resolution `7` creates a `49 x 49` ocean grid and `6` split levels:
  `49, 24, 12, 6, 3, 1`.

The runtime builds all available dyadic levels because the support-aware sampler needs a full
wavelength ladder from the simulation floor up to the whole-domain low-pass limit. The modifier's
adaptive mesh later clamps the requested quadtree depth against these available levels and against
grid divisibility constraints. Cycles upload also has its own fixed `OCEAN_SPLIT_MAX_LEVELS` cap.

Level semantics:

- split level `0` is the full-resolution, full-spectrum simulation result
- higher split levels are progressively coarser low-pass reconstructions
- the minimum supported wavelength is `2 * max(sim_cell_x, sim_cell_z)`
- split level wavelength is `min_wavelength * 2^level`

For each split level, the runtime stores:

- `size_x`, `size_y`
- the scalar support `wavelength`
- displacement fields `disp_x`, `disp_y`, `disp_z`
- reconstructed geometry normal fields `normal_x`, `normal_y`, `normal_z`
- `cumulative_disp_variance[3]`
- `cumulative_slope_moment[3]`

Levels above `0` also own reduced inverse-FFT scratch:

- `fft_in`
- `fft_out`
- `fft_plan`

Level `0` does not allocate reduced FFT scratch. It is populated by copying the current full
simulation displacement buffers into the split level, then computing normals and moments from that
full-resolution surface.

Reduced levels are generated from the same current spectrum, not from already-downsampled spatial
data:

1. For each reduced level and each displacement field, copy compatible frequency bins from the full
   `_htilda` spectrum into the reduced FFT input.
2. Apply `ocean_split_mask_weight()` to produce a smooth cumulative low-pass band for that split
   level.
3. Execute the reduced inverse FFT.
4. Copy the reduced output into the level's displacement field.
5. Reconstruct normals from finite differences of the displaced reduced surface.
6. Accumulate displacement variance and slope moments for that cumulative band.

The mask is cumulative. Higher split levels still represent the same phase-coherent ocean state, but
with progressively shorter wavelengths removed. This is why a coarser geometry leaf can still
sample a deterministic low-pass version of the exact same ocean instead of switching to unrelated
detail.

The stored moments are used by the modifier to reason about omitted content:

- `cumulative_disp_variance` stores mean squared displacement for the current cumulative band.
- `cumulative_slope_moment` stores the packed slope covariance-like moment `(xx, xz, zz)`, derived
  from reconstructed normals as `slope_x = -normal_x / normal_y` and
  `slope_z = -normal_z / normal_y`.
- The modifier computes omitted displacement variance as level `0` variance minus the candidate
  level variance.
- The modifier computes omitted slope covariance as level `0` slope moment minus the candidate
  level slope moment, then projects it to a positive-semidefinite covariance.

Those omitted moments feed the adaptive leaf blind-spot margin and the visible-footprint guard.
The modifier also samples the split levels directly for adaptive-leaf validation and final vertex
displacement.

`BKE_ocean_eval_uv_split_support()` remains the support-aware API for sampling geometry and visible
shading channels from covariance support descriptors. The current adaptive LOD mesh path usually
samples discrete runtime split levels directly for vertex displacement; the support API is still
used for non-adaptive Camera LOD fallback sampling and for shared split support semantics.

Support-aware sampling maps a covariance support descriptor to the split hierarchy by using the
minor eigenvalue of the support covariance. The resulting minor wavelength chooses a base split
level. If the support is broader or anisotropic than that base level, the sampler subtracts the
base level's isotropic variance from the covariance and applies a small anisotropic Gaussian filter
while sampling the chosen split level. That gives smooth transitions for support widths that fall
between two dyadic split levels.

The public runtime API exposes only the data needed outside `ocean.cc`:

- `BKE_ocean_split_level_count_get()`
- `BKE_ocean_split_min_wavelength_get()`
- `BKE_ocean_split_runtime_revision_get()`
- `BKE_ocean_split_runtime_level_get()`
- `BKE_ocean_split_runtime_sample_level()`
- scoped read variants for parallel modifier sampling
- `BKE_ocean_split_runtime_level_normal_data_get()` for Cycles normal texture upload

## Settings Construction

`ocean_camera_lod_settings()` builds the per-evaluation LOD settings after ocean simulation.

Important settings:

- `dense_cells_per_side = max(resolution * resolution, 4)`
- dense vertex budget is `(dense_cells_per_side + 1)^2`
- domain half extent is `0.5 * omd->size * omd->spatial_size`
- requested LOD levels are clamped by available split levels and dyadic grid constraints
- level `i` uses stride `2^i`, cell size `dense_cell_size * 2^i`, and split level `i`
- `lod_pixel_error` overrides the reprojection tolerance
- `lod_camera_full_spectrum_radius`, if nonzero, defines the camera-anchor full-spectrum radius;
  otherwise the radius defaults to `2 * dense_cell_size`

The active projection set comes from the scene camera:

- General Render mode uses the active render views.
- Stereo Dataset mode requires Stereo 3D multiview and uses the left/right stereo views.
- The LOD center is the average camera XY anchor in object space.
- The visible footprint is the union of each camera frustum clipped against the ocean domain, grown
  by a horizontal displacement guard and by the coarsest LOD cell size.

Stereo Dataset mode forces `GEOMETRY_STRICT` validation.

## Leaf Selection

Leaf selection happens in `ocean_camera_lod_build_leaves()` after settings construction and split
moment extraction. It produces a `Vector<OceanCameraLODLeaf>` for adaptive topology emission. If the
settings already require full-domain dense output, if there are no levels, if there is no valid
projection set, or if split moments are unavailable, adaptive leaf selection is skipped and the
modifier falls back through the surrounding dense/uniform paths.

The selector works over the canonical dense-cell lattice:

- `dense_cells_per_side = max(resolution * resolution, 4)`
- dense cell coordinates map linearly to `[-domain_half_extent, domain_half_extent]`
- local level `0` is the finest topology level
- local level `quadtree_levels - 1` is the coarsest active topology level
- local level `i` uses stride `2^i` dense cells per leaf edge
- the coarsest root stride is `settings.levels.last().stride`

`quadtree_levels` is clamped before selection. `ocean_camera_lod_clamp_level_count()` starts with
stride `1` and only adds another local level when the next doubled stride divides the dense grid and
would still leave at least `4` cells per side at that coarser stride. This keeps parent/child bounds
dyadic and avoids top-level grids that are too small to give the selector useful spatial variation.

The number of adaptive leaves is data-dependent:

- when adaptive selection runs, the minimum non-dense adaptive count is the root grid count,
  `(dense_cells_per_side / coarsest_stride)^2`
- maximum useful count is `dense_cells_per_side^2`, one leaf per dense cell
- if selection reaches the all-finest case, the code sets `dense_ceiling` and uses the dense mesh
  fast path instead of constructing an adaptive mesh with one face per dense cell

Each `OceanCameraLODLeaf` stores:

- `local_level_index`: topology level, with `0` as finest
- `split_level_index`: split-spectrum level used by that leaf's carrier surface
- `stride`: dense cells covered by one leaf edge
- `cell_size`: object-space length represented by that stride
- dense-grid bounds `[min_x, max_x) x [min_y, max_y)`
- `error_stats`: the last observable-error result, only meaningful after validation sampling

In normal settings, `split_level_index` numerically tracks `local_level_index`: coarser topology
uses coarser split-spectrum data, and each split child decrements both indices. The child clamp
`max(parent.split_level_index - 1, 0)` prevents the spectrum level from going below the full
spectrum level.

Root construction covers the full ocean domain, not just the visible footprint. The current code
sets the root region to the domain min/max, converts that to dense-grid root indices, and appends
one root leaf for every coarsest-stride tile. Invisible or irrelevant roots are therefore still
represented, but they are allowed to remain coarse.

Selection then runs as a breadth-first frontier:

1. Evaluate every leaf in the current frontier in parallel with
   `ocean_camera_lod_leaf_needs_split()`.
2. Append accepted leaves to the final leaf list.
3. Replace split leaves with four children split at the midpoint in dense-grid X/Y.
4. Repeat until the frontier is empty.

A child has `local_level_index - 1`, `split_level_index - 1` clamped to `0`, `stride / 2`,
`cell_size / 2`, and one of the four quadrant bounds. Children are appended in southwest,
southeast, northwest, northeast order.

For a candidate leaf, `ocean_camera_lod_leaf_needs_split()` applies this decision order:

1. Convert the leaf dense bounds to object-space XY bounds.
2. Check whether the leaf can split. A leaf can split only when its local level is above `0`, its
   stride is above `1`, and both dense-grid dimensions are at least `2`.
3. If the leaf intersects the full-spectrum camera anchor radius and can split, split it. This
   forces the anchor neighborhood down to full-spectrum geometry.
4. If the leaf cannot split, accept it.
5. Check relevance. A leaf is relevant if it intersects the anchor, or if it intersects the grown
   visible footprint AABB and at least one active projection after applying the visible footprint
   guard. If it is not relevant, accept it coarse.
6. Compute the projected resolvability bound. If the current split level or the current polygon
   size is too coarse for that bound, split it.
7. If the leaf is not directly visible, is not anchor-protected, and validation is not
   `GEOMETRY_STRICT`, accept it.
8. Sample observable error against the dense same-state reference. Split unless the sampled error
   plus statistical margin is within tolerance.

The projected resolvability bound is a cheap pre-validation gate. It samples planar pixel
sensitivity at the region corners and center using an offset of
`max(0.25 * max(leaf.cell_size, dense_cell_size), 1e-4)`. From the worst sensitivity it computes:

- `meters_for_pixel_error = reprojection_tolerance_px / pixel_sensitivity`
- `resolvable_wavelength = 2 * meters_for_pixel_error`
- `allowed_split_level = largest split level whose wavelength is still <= resolvable_wavelength`
- `max_cell_size = max(0.5 * resolvable_wavelength, dense_cell_size)`

The leaf passes this gate only if `leaf.split_level_index <= allowed_split_level` and
`leaf.cell_size <= max_cell_size`. The first condition prevents a too-low-pass spectrum from
discarding wavelengths that the camera could resolve. The second condition prevents a full-spectrum
but under-tessellated polygon from passing only because its split level is fine enough.

Observable validation compares the candidate rendered carrier against split level `0` at the same
coordinates:

- the reference sample is full-spectrum split level `0`
- the candidate leaf samples its four corners at `leaf.split_level_index`
- interior candidate samples are interpolated over the emitted quad carrier
- interpolation follows the render mesh's lower-left to upper-right diagonal

The regular validation stencil is `3 x 3`: `u, v` in `{0, 0.5, 1}`. In normal camera-observable
mode and Stereo Dataset mode, a sample only counts if the full-spectrum reference point projects
into at least one active view. General Render `GEOMETRY_STRICT` keeps non-visible samples on the
geometry path after the direct-visibility gate.

If the regular stencil produces no usable samples and the hard cap was not already exceeded, the
code clips the leaf rectangle against each active projection. It then tries the clipped polygon
centroid followed by the clipped polygon vertices. This catches thin slivers of visibility that a
fixed `3 x 3` stencil can miss.

Every accepted sample accumulates sampled error, statistical margin, and combined sampled-plus-margin
error. The stored stats contain RMS and max values for:

- position error in object-local meters
- reprojection error in pixels
- camera-space depth error
- geometric normal angle
- temporal reprojection proxy
- grazing-view reprojection amplification

Sampling stops early when the combined max error exceeds the hard cap. The hard cap is
`tolerance * hard_cap_scale` for each metric.

The blind-spot margin accounts for frequencies omitted by the candidate split level between sampled
points. Its radius is `0.25 * max(leaf_width, leaf_height)`. It uses the omitted displacement
variance and omitted slope covariance described in the split-spectrum section:

- local position margin is `min(total_displacement_rms, slope_rms * blind_spot_radius)`
- reprojection and depth margins are derived from that local position margin
- normal margin is derived from a curvature bound
- temporal and grazing margins are skipped in Stereo Dataset mode

Tolerance checks require both RMS and max values to pass. General Render validates reprojection,
depth, normal, temporal, grazing, and a softened position gate. `GEOMETRY_STRICT` uses the strict
position tolerance. Stereo Dataset mode forces `GEOMETRY_STRICT` and skips temporal/grazing terms.

If validation produces no usable samples, the leaf splits only when validation is
`GEOMETRY_STRICT` and the leaf is directly visible. Otherwise it is accepted.

After selection, `ocean_camera_lod_balance_leaves()` enforces 2:1 edge balance. It builds a dense
cell owner map, checks the south/east/north/west neighbors of each accepted leaf, and splits any
splittable leaf whose edge neighbor is more than one local level finer. Balance runs for up to
`32` passes and only refines accepted leaves; it never coarsens them.

If selection proves that the whole domain must refine to the finest grid, `dense_ceiling` is set and
`doOcean()` routes to dense reference geometry instead of spending time assembling adaptive topology.

## Topology Emission

`generate_ocean_geometry_camera_lod()` emits one face per accepted balanced leaf.

The topology is assembled over the canonical dense lattice:

- every emitted vertex is keyed by dense-grid `(x, y)`
- shared leaf corners reuse the same vertex
- the emitted vertex count is asserted to stay at or below the dense reference budget
- each leaf face normally has four corners
- a coarse leaf bordering finer leaves receives extra edge corners, producing stitched n-gons with
  up to eight corners

The face corner order walks south, east, north, then west edges. Extra split positions are inserted
where finer neighbors touch a coarse edge. This creates crack-free topology without forcing the
whole domain to the finest resolution.

Point metadata:

- `ocean_camera_lod_level`
- `ocean_camera_lod_split_level`
- `ocean_camera_lod_morph`
- `ocean_camera_lod_radius`

Face metadata:

- `ocean_camera_lod_leaf_id`
- `ocean_camera_lod_leaf_level`
- `ocean_camera_lod_leaf_split_level`
- `ocean_camera_lod_cell_size`

Mesh string properties:

- `ocean_camera_lod_contract = adaptive_leaf_v1`
- `ocean_camera_lod_layout = adaptive_leaf`

`ocean_camera_lod_radius` is only a debug distance from the camera anchor. It does not mean the
current layout is radial.

## Seam Morphing

Vertices near transitions from coarse leaves to finer neighbors can carry a morph factor.

- morph factor `1` means use the vertex's own split level
- morph factor below `1` blends toward the next finer split level
- intermediate edge vertices introduced only for stitching usually use morph factor `0`
- morph width is `2 * leaf.cell_size`

During displacement, if `split_level > 0` and `morph_factor < 1`, the modifier samples both the
current split level and `split_level - 1`, then linearly blends displacement and normal.

This smooths displacement across stitched transitions while keeping the topology local to leaves.

## Final Displacement And Attributes

After topology generation, `doOcean()` saves the undisplaced reference positions and displaces every
vertex.

For adaptive LOD vertices:

1. Convert the reference position to ocean UVs.
2. Read the vertex split level and morph factor.
3. Build an isotropic geometry support descriptor from the larger of:
   - `2 * cell_size`
   - `min_wavelength * 2^split_level`
4. Sample the split runtime level for displacement and geometry normal.
5. If required, morph-blend against the next finer split level.
6. Apply vertical displacement and optional choppy horizontal displacement.
7. Write Camera LOD and ocean attributes.

The attributes required by Cycles and diagnostics are:

- `ocean_ref_coord`
- `ocean_ref_uv`
- `ocean_geometry_normal`
- `ocean_geometry_support_covariance`
- the Camera LOD point attributes listed above

For non-Cycles output, the modifier writes a point-domain `custom_normal` attribute from the
geometry-band normal. For Cycles, this extra custom-normal layer is skipped because Cycles consumes
the ocean attributes directly.

## Cycles Shading Path

Cycles receives two kinds of Camera LOD data:

- point attributes on the evaluated mesh, written by `doOcean()` in
  `source/blender/modifiers/intern/MOD_ocean.cc`
- object-level split runtime resources, uploaded by `sync_ocean_split_resources()` in
  `intern/cycles/blender/mesh.cpp`

The point attributes carry the explicit geometry surface:

- `ocean_ref_coord`: undisplaced object-local reference coordinate
- `ocean_ref_uv`: ocean UV corresponding to that reference coordinate
- `ocean_geometry_normal`: geometry-band normal in canonical ocean coordinates
- `ocean_geometry_support_covariance`: geometry support covariance in ocean X/Z coordinates

`intern/cycles/blender/mesh.cpp` maps those attribute names to Cycles standard attributes:

- `ATTR_STD_OCEAN_REF_COORD`
- `ATTR_STD_OCEAN_REF_UV`
- `ATTR_STD_OCEAN_GEOMETRY_NORMAL`
- `ATTR_STD_OCEAN_GEOMETRY_SUPPORT_COVARIANCE`

The standard attribute names are registered in `intern/cycles/scene/attribute.cpp`. The ref-UV
attribute is marked as smooth face-varying data for subdivision when subdivision export is active.

The split runtime resource upload is object-scoped:

1. `blender_object_ocean_split_modifier()` finds the live evaluated Ocean modifier on the depsgraph
   object, not on the persistent original object.
2. The modifier must have `MOD_OCEAN_USE_CAMERA_LOD`, must be in `MOD_OCEAN_GEOM_GENERATE`, and
   must have a valid live `Ocean` runtime.
3. `sync_ocean_split_resources()` clears stale pre/post resource arrays and skips upload entirely
   when no valid modifier is found.
4. Stereo Dataset mode returns early and intentionally uploads no residual split resources.
5. `sync_ocean_split_runtime_step_resources()` uploads up to
   `min(BKE_ocean_split_level_count_get(), OCEAN_SPLIT_MAX_LEVELS)` levels.

`OCEAN_SPLIT_MAX_LEVELS` is `16` in `intern/cycles/kernel/types.h`. This is a fixed Cycles kernel
object storage limit, separate from the Ocean runtime's available split-level count.

For each uploaded level, Cycles stores:

- an image handle for the level normal texture
- cumulative slope moments
- resolution in X/Y
- texture cell size in X/Z
- the shared minimum split wavelength

The image loader is `BlenderOceanSplitSlopeLoader`. Despite the Cycles-side `slope` naming, the
loader asks Blender for normal data through `BKE_ocean_split_runtime_level_normal_data_get()`.
That API returns float4 pixels:

- `x = normal_x`
- `y = normal_y`
- `z = normal_z`
- `w = 1`

The image is configured as float4, linearly interpolated, repeat-wrapped, and tagged with
`omd->time` as the image frame. The shader converts sampled normals to slope with
`ocean_split_normal_sample_to_slope()`.

The Cycles `Mesh` stores these arrays in `intern/cycles/scene/mesh.h`:

- `ocean_split_slope_images`
- `ocean_split_slope_images_pre`
- `ocean_split_slope_images_post`
- `ocean_split_cumulative_slope_moments`
- `ocean_split_cumulative_slope_moments_pre`
- `ocean_split_cumulative_slope_moments_post`
- `ocean_split_resolution_x/y`
- `ocean_split_cell_size_x/z`
- `ocean_split_min_wavelength`

`intern/cycles/scene/object.cpp` copies those mesh arrays into each `KernelObject`:

- `ocean_split_level_count`
- `ocean_split_min_wavelength`
- `ocean_split_resolution_x/y[OCEAN_SPLIT_MAX_LEVELS]`
- `ocean_split_cell_size_x/z[OCEAN_SPLIT_MAX_LEVELS]`
- current/pre/post texture slots
- current/pre/post cumulative slope moments

All kernel object slots are initialized to empty defaults first. A mesh contributes split data only
when it has at least one valid current split image.

Motion blur uses the same resource path. During Cycles motion sync, `sync_motion()` captures split
resources for the pre and post motion steps into the `_pre` and `_post` arrays. In the shader,
`ocean_split_time_pair()` chooses the texture pair:

- for `sd->time <= 0.5`, interpolate pre to current if pre is available
- for `sd->time > 0.5`, interpolate current to post if post is available
- otherwise sample the current texture twice

The shader implementation lives in `intern/cycles/kernel/closure/bsdf_ocean.h`.

`ocean_split_visible_normal()` is the active normal reconstruction entry point. It first reads the
geometry-band normal from `ocean_geometry_normal` and transforms it from canonical ocean coordinates
to world space. Then it calls `ocean_split_visible_slope()`.

`ocean_split_visible_slope()` requires all of the following:

- object split runtime data in the `KernelObject`
- a nonzero canonical `ocean_geometry_normal`
- `ocean_ref_uv`
- `ocean_geometry_support_covariance`

The geometry support covariance is projected to a positive-semidefinite tensor, then mapped to a
base split level with `ocean_split_support_base_level()`. If the explicit geometry already maps to
base split level `0`, the helper returns `false`; Cycles leaves the normal path alone because the
geometry is already full spectrum.

When the explicit geometry is coarser than split level `0`, the current shader path samples split
level `0` at `ocean_ref_uv`. It converts the sampled normal texture to a visible slope, interpolates
motion texture samples if needed, reconstructs a canonical normal from `(-slope_x, 1, -slope_z)`,
and transforms that normal to world space.

OSL closures use this through `osl_ocean_default_normal()` in
`intern/cycles/kernel/osl/closures_setup.h`. If a material supplies its own normal, the helper
retargets that material normal from the geometry-band base normal onto the reconstructed
full-spectrum ocean normal with `ocean_split_retarget_material_normal()`.

`ocean_split_unresolved_covariance()` currently returns `false` and writes zero covariance. The
render path reconstructs the dense/reference slope field directly from split level `0`, so there is
no additional omitted shading band to fold into microfacet roughness. Adding camera-footprint
roughness here would bias brightness relative to the dense reference render.

This means General Render mode can render reduced explicit geometry while shading with same-state
full-spectrum normal detail in Cycles. Stereo Dataset mode uses only the explicit geometry-band
surface and deliberately omits residual split shading resources.

## Caches

There are two modifier-side Camera LOD caches:

- a full evaluated result cache keyed by frame, camera, object, scene, and ocean settings
- a topology cache keyed similarly but with frame/time zeroed, so topology can be reused across
  time changes when the layout does not depend on the simulated phase

Dense full-grid templates are also cached by dense cell count and domain extent.

The cache keys include Camera LOD settings, camera transforms and lens parameters, object transform,
render resolution, multiview state, and ocean simulation parameters that affect the layout or
displacement.

## Validation Coverage

The main regression coverage is in `tests/python/physics_ocean_split_spectrum.py`, with helpers in
`tests/python/modules/ocean_camera_lod_metrics.py`.

The tests cover:

- required Camera LOD attributes
- camera-anchor tracking of split level `0`
- dense vertex budget invariant
- large-footprint adaptive leaf layout metadata
- reference validation against dense same-state geometry
- strict mode behavior
- conservative visible coverage
- dense fallback for foam/spray
- Stereo Dataset mode and cache invalidation
