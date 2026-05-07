# Camera LOD Quadtree Replacement Design

## Summary

Blender's current ocean camera LOD implementation is center-driven. It computes a single
`settings.center`, builds nested square regions around that center, and then chooses split levels
from those concentric regions.

That behavior is not aligned with the intended product behavior:

- mesh density should follow the camera-visible ocean footprint
- the finest geometry should land on the nearest visible water
- geometry behind the camera or outside the visible footprint should not consume the dense budget

This design replaces the centered-region layout with a quadtree over the ocean plane. Refinement is
driven directly by camera visibility and stereo observability error, while retaining the existing
split-spectrum displacement backend and camera-projection math.

## Goals

- Replace the current center-based camera LOD layout entirely.
- Make emitted geometry density follow the visible ocean footprint rather than concentric regions.
- Concentrate the finest cells on the nearest visible water and other high-error visible regions.
- Preserve the existing split-spectrum evaluation model where possible.
- Produce crack-free topology with bounded neighbor resolution differences.
- Provide a debug contract that reflects footprint-adaptive cells instead of radial bands.

## Non-goals

- Retuning the current centered-region solver.
- Keeping `settings.center` as the primary layout primitive.
- Changing the ocean spectrum model itself.
- Preserving radius-based debug semantics.
- Solving every downstream StereoOcean adaptation in the same patch.

## Current Failure Mode

The current solver uses the midpoint of the camera-visible footprint AABB as the camera-LOD center
when a visible footprint exists. In wide oblique views this can place the finest geometry far from
the camera-support point, even though the camera is the driver of the visible footprint.

Observed behavior on the 3 km StereoOcean case:

- rig anchor / camera XY near `(1200, -1200)`
- support center near `(1163, -1163)`
- visible footprint spanning almost the full domain
- chosen `settings.center` near `(-146, 141)` because it is the midpoint of the footprint AABB,
  snapped to the LOD grid

This is not a bug in labeling or topdown visualization. It is a direct consequence of the current
center-based layout policy.

## Design Overview

### Core model

Replace the centered-region layout with an adaptive quadtree on the ocean reference plane:

- the quadtree lives in object-local ocean-plane XY
- roots cover the visible-footprint union AABB, clipped to the ocean domain
- each node represents one candidate geometry patch
- each accepted leaf owns a local grid patch and one geometry split level

There is no global LOD center and no concentric region construction.

### Refinement rule

Refinement is driven by visibility and camera-observable error:

- invisible cells are discarded
- visible cells are sampled against active camera views
- a cell is accepted if all relevant samples satisfy the current tolerance policy
- otherwise it is split into 4 children
- recursion stops when the cell passes or reaches the finest allowed depth

This retains the current observability logic and tolerance model, but applies it per cell rather
than per centered shell.

## Node Selection

### Root construction

Start from one or more root nodes that cover the visible-footprint union AABB. The solve does not
need to consider the full ocean domain if most of it is not visible.

### Per-node evaluation

Each node is tested with a fixed sample stencil:

- center sample
- edge midpoint samples
- corner samples

For each sample:

- reject it if it is outside the ocean domain
- determine whether any active camera view can see it
- if visible, evaluate the current cell size against the stereo/camera error tolerances

Acceptance policy:

- if no samples are visible, prune the node
- if all visible samples pass, accept the node as a leaf
- if any visible sample fails, split the node

This makes refinement conservative near horizons, footprint boundaries, and mixed-visibility cells.

## Topology And Stitching

### 2:1 balancing

After the error-driven refinement pass, enforce a 2:1 balance rule:

- adjacent leaves may differ by at most one quadtree level
- if a leaf touches a much finer neighbor, subdivide it until the constraint is satisfied

This prevents uncontrolled T-junctions and keeps seam handling bounded.

### Leaf emission

Emit one regular grid patch per accepted balanced leaf:

- the patch is generated in reference-plane XY
- leaf depth determines the geometry cell spacing
- leaf depth also maps to the owning camera LOD level

### Seam handling

Where a coarse leaf borders finer leaves:

- generate stitched edge topology rather than forcing equal resolution globally
- allow only one-level transition seams after balancing
- keep seam logic local to shared leaf edges

This preserves local adaptation without collapsing the quadtree back into concentric layouts.

## Split-Spectrum Evaluation

The displacement backend should remain split-level based.

For each emitted vertex:

- determine its owning leaf
- map the leaf depth to the chosen split level for geometry ownership
- sample the existing split-spectrum runtime at that split level

For one-level seam transitions:

- retain or adapt the current morph/blend behavior where needed for smooth displacement and normals
- keep morph semantics local to seam neighborhoods rather than radial transition bands

The major rewrite is in layout and topology generation, not in ocean-spectrum evaluation.

## Debug And Attribute Contract

### Keep

- `ocean_camera_lod_level`: emitted geometry owner level
- `ocean_camera_lod_split_level`: sampled spectral split level
- `ocean_camera_lod_morph`: local seam/blend state where used

### Replace

`ocean_camera_lod_radius` is no longer semantically valid once layout is not center-based.

Replace or supplement it with:

- `ocean_camera_lod_cell_size`
- leaf or node identifiers sufficient to reconstruct the emitted quadtree
- optional debug data for leaf bounds and seam edges

Topdown diagnostics should visualize:

- leaf bounds
- active visible-footprint coverage
- seam/transition edges

They should stop presenting camera LOD as radial bands.

## Compatibility And Migration

This change replaces the current implementation rather than preserving it behind a mode switch.

Compatibility implications:

- any consumer assuming radial LOD semantics must be updated
- StereoOcean should treat camera LOD as adaptive visible-footprint cells, not as shells around a
  center
- existing radius-based metrics and overlays should migrate to cell-size and leaf-boundary views

The Blender-side patch should preserve attribute names that still make semantic sense, but the
debug contract must explicitly move away from center/radius interpretation.

## Testing

### Unit

Add focused tests for:

- visibility pruning
- error-driven node splitting
- finest-depth stopping behavior
- 2:1 balancing
- seam topology generation between one-level neighbors

### Geometry regressions

Add Blender regression coverage for:

- single-camera oblique footprint
- stereo dataset footprint union
- wide corner-looking shots over large domains
- cases with disconnected visible islands

### Acceptance criteria

The replacement is successful when:

- the densest geometry follows the visible ocean footprint
- the finest cells land on the nearest visible water unless another visible region has higher error
- no fine cells are emitted behind the camera solely because of a centered layout artifact
- moving the camera changes the refined footprint directly
- the emitted topology is crack-free after balancing and stitching
- no single global center is needed to explain the layout

## Risks

- Quadtree refinement can increase topology-generation complexity and debug difficulty.
- Seam stitching needs careful handling to avoid displacement discontinuities.
- The current debug and metrics ecosystem assumes radial concepts that will become invalid.
- Large visible-footprint unions could still generate many leaves without careful acceptance and
  pruning heuristics.

## Recommended Implementation Order

1. Introduce quadtree node selection driven by current visibility/error tests.
2. Add 2:1 balancing over accepted leaves.
3. Replace centered mesh emission with leaf-based patch emission.
4. Add seam stitching and local morph handling.
5. Replace radial debug attributes with cell-size and leaf-boundary diagnostics.
6. Update downstream tests and consumers after the Blender-side geometry contract is stable.
