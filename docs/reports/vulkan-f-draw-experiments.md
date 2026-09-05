# Vulkan F draw-dependent experiments

The test component pins Vulkan indirect record ABIs: non-indexed commands are 16 bytes and indexed commands are 20 bytes. It validates count, stride, four-byte alignment, zero cases, and the full computed byte range. Tier2 input uses complete `NodeRef` identity plus validated private bucket/package slots.

The physical GPU tests cover indirect dispatch, cull/compact, indexed-address execution, and Tier2 bucket/fill. The production plan-driven path additionally consumes GPU-authored direct or indexed draw commands through the formal Raster pipeline, with explicit compute-write to indirect-command/vertex-input synchronization and attachment readback barriers.

Linux llvmpipe and Khronos validation pass all registered modes with no VUID. The plan-driven Tier2 test verifies direct and indexed A,A,A,B selection, output pixels, sealed publication order, and pre-device rejection of unauthorized or mixed-ABI plans.
