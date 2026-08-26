# ADR-049: F4 — Depth + Real PSO

Status: Accepted

## Context

F3.5 (ADR-048) made a color-only raster task available through the public C
ABI. F4 adds the first depth path and makes native pipeline state observable:
an application must be able to request a depth attachment and depth comparison
without reaching behind `vg.h`, while Reference remains the oracle for the
built-in raster path.

ADR-048 also documented that growing raw-array `VgTaskRecord` is unsafe for a
stale-header client: the library's `tasks[i]` stride can exceed the caller's
allocation stride. F4 requires new per-task fields, so that exception cannot
be repeated.

## Decision

1. **Publish C ABI v1.4 and a separate fixed layout.** `VgTaskRecordV2` is a
   new, complete raw-array element type and `VgApi::taskGraphAppendV2` is
   appended at the v1.4 function-table boundary. `VgTaskRecord` and
   `taskGraphAppend` permanently retain v1.3 layout and behavior. Future
   growth must add another versioned entry point (or explicit element stride),
   never append fields to either raw-array record.
2. **Add depth format and state.** `VG_PIXEL_FORMAT_DEPTH32_FLOAT` may only
   be used for a depth Attachment facet. A V2 raster record names that facet
   and carries test-enable, write-enable, and one of all eight explicit depth
   compares: Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual,
   Always. A depth-enabled task requires a depth facet; its dimensions,
   layers, and sample count must match the RGBA8 color attachment.
3. **Fix F4 depth behavior.** Each task clears depth to `1.0` and stores it.
   F4 is single-sample, non-indexed TriangleList with one RGBA8 color and one
   Depth32Float attachment. It excludes load/store choice, stencil, blend,
   MSAA, indexed draws, and compute+raster mixed submissions. Test/write
   disabled uses `Always` as the default comparison behavior.
4. **Upgrade the vertex contract.** Built-in raster and
   `"vg.msl.raster/v1"` use tightly packed `{ float x, y, z, u, v }` vertices;
   MSL declares `packed_float3 position; packed_float2 uv`. `z` is finite normalized
   depth in `[0,1]`; invalid input is rejected. This is an intentional F3
   source/binary break: user MSL and the producer of its vertex data must be
   rebuilt together. Restricted MSL envelopes must explicitly declare
   `"vertex_abi":"vg.raster.vertex.xyzuv-packed/v1"`; missing or different
   values are rejected before compile/submit so an old F3 xyuv producer cannot
   be silently interpreted with F4's 20-byte stride.
5. **Use real native depth/PSO state.** Metal's pipeline key includes depth
   attachment format and depth test/write/compare state. It creates and caches
   `MTLDepthStencilState` and binds it on the render encoder. Viewport and
   tint remain dynamic/non-keyed. Reference owns per-sample depth, barycentric
   depth interpolation, compares, and optional writes for built-in shaders;
   restricted user MSL remains `HostAssisted` and is not a Reference pixel
   oracle. Vulkan remains compile-review-only and reports raster unsupported.

## Consequences and evidence

`vgGetApi` continues append-only size negotiation: a v1.3 request ends before
`taskGraphAppendV2`; a v1.4 request must provide the full v1.4 table and gets
the new pointer. This preserves callers using old tables and, unlike ADR-048's
plain append, avoids an array-stride reinterpretation.

Required evidence is: Reference tests for all compares and depth state;
negative tests for invalid format/facet/shape/enum and invalid z; public C ABI
V2 append coverage; Metal vertical-slice color/depth equivalence plus cache
hit/miss evidence; and Vulkan compile-review evidence that F4 is still
unsupported rather than silently degraded.
