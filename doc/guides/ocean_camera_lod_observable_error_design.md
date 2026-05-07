# Ocean Camera LOD: Observable-Error Representation Model

## Reference Surface

The reference object is one displaced ocean surface sampled from one shared FFT state at one time.

- There is one canonical spectral realization.
- There is one phase-coherent displaced surface derived from that realization.
- Every channel in the LOD system must approximate that same surface rather than introduce unrelated detail.

The dense reference is the same-state generate-mode ocean sampled at the simulation-resolution floor over the full object-space footprint.

## Validity Policies

The current implementation distinguishes two explicit correctness targets.

- `CAMERA_OBSERVABLE`
  - Guarantees bounded error for the current camera-conditioned observable bundle relative to the dense same-state reference.
  - The enforced bundle is reprojection, depth, geometric normal, temporal reprojection proxy, grazing/silhouette risk, and a buffered world-space position soft cap near the current visible footprint.
  - This mode is honest about being view-conditioned. It does not claim that off-camera world-space surface position is globally faithful.
- `GEOMETRY_STRICT`
  - Adds a hard world-space position gate over the current camera-relevant footprint.
  - This is still not a claim of full-domain equality to the dense surface. It is a stricter geometry-faithful approximation over the region that can currently affect visibility, occlusion, reprojection, or near-future LOD decisions.
  - If that footprint cannot be represented within tolerance at a coarse band, the allocator must remain dense.

Both policies preserve the same shared FFT realization, world/object-space anchoring, and channel split. The difference is which error contract is enforced.

## Observables To Preserve

The approximation target is not only beauty rendering. The representation must stay valid for:

- RGB appearance
- world-space surface position
- reprojection under camera motion
- depth and stereo disparity
- surface normals and slope-sensitive shading
- silhouette and grazing-view occlusion behavior

Geometry support is therefore chosen by minimizing cost subject to bounded error in those observables relative to the dense same-state reference.

## Channel Partition

The shared state may be partitioned into three channels.

- Geometry-resolved band
  - Carries all content whose omission would materially change explicit surface position, depth, disparity, reprojection, silhouette, occlusion, or geometry-sensitive normals.
- Deterministic shading-resolved band
  - Carries omitted content reconstructed from the same FFT state only when leaving it out of explicit surface intersection still stays within the geometry-error tolerances.
  - This channel may change shading normals and subpixel appearance, but it must remain phase-coherent with geometry and must not double count any band already carried by geometry.
- Statistical unresolved tail
  - Only represents the omitted same-state tail below the effective output bandwidth.
  - Must be derived from omitted simulated content, not from unrelated procedural noise.

## Invariants

- One shared phase-coherent FFT state.
- Stable world/object-space anchoring.
- Camera motion changes only representation allocation, not ocean placement.
- No band is counted twice across geometry, deterministic shading, and statistical tail.
- No frequencies finer than the simulation bandwidth are represented.
- Any omitted geometry band must be justified by an observable error bound, not by visual plausibility alone.

## Boundary Criterion

The LOD allocator operates on candidate spatial regions and candidate geometry cutoffs.

For each candidate region:

1. Choose a candidate geometry cutoff bounded by the simulation floor.
2. Compute the omitted spectral tail above that cutoff from split moments derived from the shared FFT state.
3. Estimate the impact of that omitted tail on:
   - projected surface position / reprojection
   - camera-space depth, with stereo disparity scaling from the same term
   - normal / slope error
   - temporal instability under view change
   - grazing-view / silhouette risk
4. Accept the coarsest candidate geometry support whose sampled region error stays below tolerance.

The acceptance decision is not based on sampling alone:

- sampled dense-reference comparisons measure the current observable error at explicit region points
- omitted-band moments provide a conservative local blind-spot margin between samples
- the allocator accepts a candidate only when the sampled error plus the omitted-tail margin stays below tolerance under the active validity policy

This replaces projected wavelength as the primary decision variable. Wavelength still appears only as a bookkeeping index into the shared split hierarchy and as a ceiling imposed by the simulation bandwidth.

## Error Model Used By The Modifier

The modifier uses sampled region statistics plus a local omitted-tail bound instead of a single worst-case probe.

- Each candidate split level is compared directly against the dense same-state split level at sampled points in the candidate region.
- Reprojection error is measured from the direct projected difference between the candidate surface point and the dense same-state reference point.
- Depth error is measured directly in camera space against that same reference point.
- Geometric-normal error is measured directly from the candidate/reference geometry normal difference.
- Omitted displacement and slope moments are still tracked from the shared FFT split hierarchy and are used for instrumentation, temporal-frequency estimation, and a blind-spot margin between sample points.
- Temporal error is estimated by scaling the sampled reprojection error by an effective omitted-band frequency derived from omitted displacement and slope moments.
- Grazing risk amplifies the sampled reprojection error by view-angle sensitivity.
- World-space position error is enforced over a camera-relevant footprint instead of the whole domain.
  - The footprint is the current plane-projected visible footprint expanded by a buffer derived from the candidate geometry cutoff wavelength plus omitted horizontal displacement RMS.
  - This buffer protects near-future visibility and grazing-view occlusion without forcing the entire ocean domain to remain dense.
  - `CAMERA_OBSERVABLE` treats position as a soft gate over that footprint.
  - `GEOMETRY_STRICT` treats position as a hard gate over that footprint.

Region acceptance uses sampled statistics over the candidate region plus an omitted-tail margin rather than "one bad probe keeps the whole ring dense".

## Normal Semantics

The system carries two different normal notions, and they must not be conflated.

- Geometric normal
  - The normal of the explicitly intersected geometry band.
  - This is the only normal that is guaranteed to match geometry-space depth, position, and stereo intersection.
  - The modifier exports this as `ocean_geometry_normal`, and non-Cycles renderers fall back to this geometry-band normal.
- Apparent / full-band normal
  - The normal after adding deterministic same-state residual detail in shading.
  - This is valid for RGB and apparent shading detail, but it is not a claim that the explicit geometry intersection, depth, or stereo surface includes that residual band.
  - In the current architecture this normal is reconstructed inside the Cycles ocean path and is not exported as a general geometry attribute.

Validation must therefore state which normal it compares:

- geometry-faithfulness tests compare geometric normals
- beauty / apparent-shading tests compare RGB and, where available in the renderer, apparent normals separately

## Current Approximation Boundaries

The current refactor keeps the existing dyadic nested mesh layout as a carrier because it is already object-anchored and phase-coherent. The principled change is the support decision logic:

- region boundaries are chosen from omitted-band observable error
- geometry cutoffs are mapped onto the existing split hierarchy
- deterministic shading continues to use the same shared-state residual hierarchy

If a future implementation replaces the carrier layout, the same observable-error objective and channel invariants should remain the governing model.

## Honest Limitations

- The first pass uses sampled region estimates rather than a closed-form integral over the full region.
- The current blind-spot margin uses omitted displacement and slope moments as a local conservative bound; it is not yet a full displaced-surface extremum proof.
- Depth is bounded directly in camera space; stereo disparity scales from the same quantity and baseline/focal length.
- The strict position gate is footprint-limited rather than whole-domain. This is intentional: a full-domain hard gate would collapse to dense geometry almost everywhere and would not reflect which part of the surface can affect current outputs.
- Apparent/full-band normals are currently explicit only inside the Cycles path. The modifier exports only geometry-band normals.
- If any required error term is still missing in code, leave an explicit TODO naming that missing bound instead of silently falling back to projected wavelength heuristics.
