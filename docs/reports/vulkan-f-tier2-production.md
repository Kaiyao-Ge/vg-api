# Vulkan F Tier2 production integration

Status: complete and integrated in the production Vulkan Stage 6/7 path.

Core now seals an explicit `tier2_selection_nodes` fact in `ExecutionPlan`. Assembly verifies complete authorized `NodeRef` values and derives `IndirectTier2Select`; Vulkan Stage 6 refuses unauthorized scheduled Raster tasks, non-Raster authority, empty selection, and incompatible indexed/non-indexed command ABIs before GPU or content-epoch side effects.

Stage 7 derives every physical selection record from the immutable scheduled Raster task. GPU bucket matching compares complete `NodeRef` index and generation. The GPU-written match buffer feeds the fill pass, which authors or zeros the actual indirect draw commands; the host does not read back counts or re-encode commands. Explicit compute-write to indirect-command and draw-consumer barriers order the passes. The lowering report classifies host authorization plus GPU bucket/fill consumption as `EmulatedDevicePass`.

`Capability::IndirectTier2Select` is advertised only with the complete Raster gate. The real plan test covers repeated/skewed A,A,A,B direct and indexed draws, exact publication order and pixels, stale or unauthorized selection, and mixed ABI rejection without side effects. Khronos validation reports no VUID.
