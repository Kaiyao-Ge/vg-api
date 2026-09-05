# Vulkan E2 restricted user-raster import

Status: complete and integrated in the production Vulkan Raster path.

Vulkan uses the versioned `vg.glsl.raster/v1` contract; `vg.msl.raster/v1` remains Metal-only and rejects before device side effects. The API and IR preserve the immutable root schema, vertex and fragment entry names, packed xyzuv ABI, and source. The Vulkan validator requires `#version`, both stage guards and declared entries, forbids an application-defined `main`, rejects MSL markers and oversized source, then compiles each guarded stage through `glslc` with a generated wrapper entry.

`UserRasterSpirvCache` stores device-independent SPIR-V by a stable source/entry/ABI key. Device-scoped shader modules and graphics pipelines use the same formal descriptor, SceneRoot, attachment, depth, and cache lifecycle as built-in Raster. The resolved Node package selects the caller shader; the backend never substitutes the built-in shader or translates MSL.

`Capability::UserShaderImport` is advertised only inside the complete Raster device gate. The registered `vertical-slice.vulkan.user-glsl-device` test assembles a real plan, compiles and draws valid user GLSL on llvmpipe, verifies pixels, Raster result, publication, and lowering events, then verifies MSL refusal with unchanged target bytes. Khronos validation reports no VUID.
