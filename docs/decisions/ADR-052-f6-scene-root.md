# ADR-052: F6 — Per-frame SceneRoot

Status: Accepted

## Decision

F6 publishes header/API v1.7 without adding a `VgApi` member, a task-record
layout, an append entry point, or a UBO object family. A caller uses the
existing `VgTaskRecordV2.root/root_generation` identity to name one stable
allocation and uses F7's bounded `writeAllocation` to update its generated
SceneRoot bytes before each submit. A sealed graph is therefore reusable
across frames without mutating task records.

`schemas/ir/scene-root-raster.vg.json` is the single source for the F6
`SceneRootRaster` layout: a column-major, affine `camera_clip_from_local[16]`
and `Material { base_color[4], VgFacetRef albedo }`. The generator emits the
public C header, host layout assertions, canonical schema ID, reflection, and
a root-relative facet relocation offset. The public header is checked against
a fresh generator output in the schema test.

Only a CodeObject/module or restricted-MSL envelope whose root schema is the
exact generated contract name `vg.scene-root.raster/v1` enables this path.
The runtime rejects stale/short root allocations, non-finite or perspective
matrices, non-finite material colors, a non-empty legacy source facet, and a
non-identity legacy tint. For a SceneRoot task, `material.albedo` is the sole
sample source and `material.base_color` is the sole tint authority; vertex,
index, color and depth facets remain the established task fields.

The fixed Metal raster ABI now binds root bytes at vertex buffer slot 1. The
built-in MSL declaration is packed to the generated 88-byte host layout and
constructs its matrix explicitly. Legacy raster work receives an explicit
identity root at that slot, so F3--F5 shader output and pipeline keys remain
unchanged. User MSL may opt into slot 1 while retaining its existing
`vg.msl.raster/v1` envelope and `HostAssisted` classification; Reference does
not claim to validate user shader pixel logic.

F6 remains 2D/orthographic: camera `w` must stay one and transformed depth
must remain finite in `[0,1]`. It does not add clipping, perspective
interpolation, blend/MSAA, arbitrary material arrays, or compute+raster mixed
submissions. SceneRoot submissions must contain raster tasks only. Existing
TaskGraph effects cannot resolve root-embedded facets at seal time, so F6
material data is a host-prepared, read-only input; cross-task producer/
consumer ordering remains explicit until resource effects can be resolved into
an immutable derived submit graph.

Capture v1 cannot snapshot/reacquire a `FacetRef` embedded in root bytes.
Rather than replaying a stale token, it explicitly refuses SceneRoot capture
replay until a capture revision consumes the generator's relocation metadata.

## Validation

The F6 C ABI test includes only public headers, uploads a generated SceneRoot,
submits one sealed graph twice after changing root bytes, and observes red then
green output through F7 readback. Schema regression verifies the public header
and the root-relative material-facet relocation offset. Reference and Metal
share the same root resolver, while Metal also retains legacy raster coverage
through its identity-root binding.
