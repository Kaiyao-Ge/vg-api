# Vulkan B: restricted pointer-graph CachedObject parity

Status: complete and integrated in the production Vulkan Stage 6/7 path.

The intended Vulkan path matches the implemented Reference/Metal E002 subset:
`load_ref`/`load_via`/`store_via` requires a verified declared single-hop
`PointerEdge`; the compiler binds the declared `load_via`/`store_via` target
directly in generated GLSL. It is a `CachedObject` package keyed by full
`NodeRef` and immutable package content. It is not raw Vulkan device-address
pointer chasing.

The integrated Stage 6 change selects `build_pointer_graph_compute_package` for
a pointer module and reports `node_compute_package` as `CachedObject`. Stage 7
rebuilds that same package from the immutable Node snapshot before dispatch,
while retaining the existing complete-NodeRef lookup, binding generation/epoch
checks, pipeline cache key, lifetime hold, and sealed schedule. Linear modules
continue through their existing package and cache behavior.

`tests/vertical_slice/vulkan_pointer_graph_test.cpp` is assembler-driven. It
requires a real Vulkan device, compares an observable nonzero `store_via` with
Reference bytes, checks CachedObject disclosure and repeat compilation, and
rejects tampered package identity before effects. Its verifier/package negatives
cover wrong declared target and pointer-package width/alignment. The
public ABI companion selects Vulkan explicitly and must not fall back to
Reference; no-device or Unsupported is failure for both GPU CTests.

Known scope boundary from ADR-028: `load_ref` bytes are deliberately elided by
the GPU CachedObject lowering. A host content write after assembly can therefore
make Reference reject its dynamic pointer-value check while Metal/Vulkan still
use the declared static target. `Arena::copy_into` changes `content_epoch`, not
the plan's topology epoch, and this package does not extend Core to freeze those
bytes. This is an existing Reference-versus-static-GPU limitation, recorded
here rather than described as a Vulkan check. Declared-edge identity, target
generation/representation lookup, package equality, and graph topology remain
the enforced B contracts.

User accepted A's explicit-int64 GLSL repair; this report does not claim a new
A run. Local Reference/compiler checks and Linux Vulkan device evidence remain
separate: no Linux SDK, adapter, `glslc`, or actual-device run was authorized
for this work package.

## Integrated validation record

The registered pointer-graph and public ABI tests execute in the Linux Vulkan
build. The final checkout passes 68/68 Linux tests and all 29 Vulkan tests on
llvmpipe under `VK_LAYER_KHRONOS_validation`, with zero validation errors or
VUID diagnostics. The ASan/UBSan Vulkan device set also passes without
sanitizer or validation findings. Reference ASan/UBSan passes 45/45 and Metal
ASan/UBSan passes 79/79.
