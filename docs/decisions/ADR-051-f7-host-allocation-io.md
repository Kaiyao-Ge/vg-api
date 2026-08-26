# ADR-051: F7 Host Allocation I/O and Readback

## Status

Accepted.

## Decision

F7 publishes API/header v1.6 and appends exactly two C ABI functions to the
`VgApi` table: `writeAllocation(arena, allocation, offset, source, size)` and
`readAllocation(arena, allocation, offset, destination, size)`. They are
synchronous bounded copies, not mappings and not new upload/readback objects.

The owner arena validates the raw `VgAllocation` pointer by address before it
is dereferenced. Non-zero ranges require non-null source/destination; overflow
and out-of-range copies fail. A zero-length copy is a valid no-op.

`Allocation::content_epoch` records byte changes independently of
`representation_epoch`: a write does not invalidate facet capability tokens,
but it refreshes stale Metal buffer/texture mirrors. Raster completion commits
color/depth bytes back into the canonical allocation before `submit` returns;
`readAllocation` consequently observes the completed result. Reference writes
the same canonical bytes directly and advances the same epoch.

## Consequences

The public ABI now closes checkpoint A's data path without introducing a map
or UBO resource family. Metal Shared storage is an implementation detail, not
the API contract. Vulkan may service canonical copy APIs while its raster task
path remains explicitly unsupported.
