# G3 private Metal implementation owners; facade remains in root CMake.
target_sources(vg_backend_metal PRIVATE
  src/backends/metal/metal_resources.mm
  src/backends/metal/metal_pipelines.mm
  src/backends/metal/metal_lowering.mm
  src/backends/metal/metal_commit.mm
  src/backends/metal/metal_encoding.mm
  src/backends/metal/metal_raster.mm
  src/backends/metal/metal_representation.mm
  src/backends/metal/metal_diagnostics.mm
  src/backends/metal/metal_shader_sources.mm
)
