# Vulkan E1 formal built-in Raster

Status: complete and integrated in the production Vulkan `DeviceHal` Stage 6/7 path.

Stage 6 compiles one immutable Raster package per complete `NodeRef`. Stage 7 walks the Core-sealed `ExecutionSchedule`, resolves that package for every Raster task, applies scheduled transitions, binds the task facets and SceneRoot, records the draw, waits for completion, reads color/depth results back, updates content epochs, and publishes each completed task in sealed order. Compute and Raster tasks share the same schedule and submission.

The built-in path supports packed xyzuv vertices, triangle-list non-indexed and `R16Uint`/`R32Uint` indexed draws, RGBA8 sample and attachment facets, optional D32 depth compare/write state, and authoritative `SceneRootRaster` camera, albedo, and tint data. Facet lifetimes remain held through GPU completion. Invalid or stale facets, malformed vertex/index ranges, unsupported formats, invalid SceneRoot data, and immutable package mismatches reject before partial Stage 7 effects.

Real llvmpipe device coverage includes public mixed Compute/Raster submission, indexed SceneRoot+D32 drawing, raster readback, publication, events, content epochs, and pre-submit negative cases. All Vulkan tests pass with `VK_LAYER_KHRONOS_validation` and no VUID diagnostics.
