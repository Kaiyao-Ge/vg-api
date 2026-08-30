# ADR-053: Device-scoped NodeRef and Multi-CodeObject TaskGraph

Status: Accepted (supersedes the single-CodeObject narrowing in ADR-044)

## Context

ADR-044 deliberately narrowed F1: a `VgTaskGraphBuilder` was created with one `VgCodeObject`, every task in the resulting graph used that object's module, and the envelope copied only the index part of an allowed `VgNodeRef` as a node-class allow-list. That was expedient for the first C ABI path, but it is no longer sound as the TaskGraph contract that the public API claims to expose.

A `VgNodeRef` is specified as an index plus generation capability token, not as a caller-selected class number (02 §1/§5 and 04 §3/§9). Dropping its generation lets a recycled node slot satisfy an envelope written for an earlier node. Tying a graph to a raw `VgCodeObject` handle also makes a valid NodeRef depend on unrelated host-handle lifetime, prevents one effect DAG from naming independently materialized programs, and makes the single-CodeObject F1 narrowing leak into the core submission model. These are authorization and lifetime defects, not a request for a new graph abstraction.

The implementation now materializes a CodeObject at load time, creates Nodes in a per-Device NodeTable, and resolves each submitted Task's complete NodeRef to an immutable program snapshot. This ADR freezes the repair and its intentionally smaller backend support envelope.

## Decision

1. **ADR-044's one-CodeObject-per-TaskGraph narrowing is superseded.** A TaskGraph is device-scoped and contains immutable Task records whose complete `VgNodeRef` selects the program. It may contain Nodes created from multiple CodeObjects on the same Device. A NodeRef is never an ISA address, CodeObject handle, or portable cross-device name.

2. **Node identity and program lifetime are device-scoped.** `VgDevice` owns the NodeTable. Creating a Node records `{index, generation, entry name, shared immutable CodeObject package}`; destroying the public CodeObject handle does not invalidate live Nodes. Destroying a Node retires its table entry and advances its generation, so lookup of the old ref fails. A submission snapshots every referenced Node and retains its package through compile and execution. CodeObject, Node, TaskGraph, and Submission host handles still follow ADR-044's same-handle destruction rule; their logical program/data lifetime is not extended by a dangling raw handle.

3. **`VgTaskGraphBuilderDesc.code_object` is a nullable, deprecated compatibility hint.** It retains its v1.1 layout and function signature. A non-null value must be a live CodeObject belonging to the supplied Device, preserving useful early same-device diagnostics for old callers; it does not restrict appended NodeRefs. `NULL` is valid and is the normal form for a multi-CodeObject graph. The runtime continues to negotiate the existing API/table version requested by the caller: this semantic clarification adds no function pointer, handle type, structure field, or extension and therefore does **not** publish v1.8. The highest real header/API version remains v1.7 (ADR-052).

4. **An ExecutionEnvelope authorizes complete device-local identities.** At envelope creation every supplied `{index, generation}` is looked up in the target Device's NodeTable. At submit, every Task's complete ref must occur in the envelope's `authorized_nodes` set when that set is non-empty. The derived index-only `authorized_node_classes` list is a backend selection/cache hint only; it is never the security or lifetime authority. Graph, Envelope, and submit Device must be the same Device. This restores the generation check ADR-044's index-only representation lost.

5. **Submission resolves programs per Node before lowering.** Core verifies materialized canonical IR at load/submit boundaries, resolves unique Nodes from the sealed graph, and carries their immutable module or restricted-MSL envelope in the ExecutionPlan. A graph with no resolvable Node program is invalid. This is an internal execution-plan refinement; it does not add a public CodeObject/Node/Graph/Envelope primitive, public handle, or C ABI entry point.

6. **Capability support is deliberately asymmetric and explicit.** The Reference backend supports a multi-CodeObject graph only when every resolved Node is canonical compute and every Task is compute; it builds a package per Node and executes the sealed graph's deterministic dependency order. Metal and Vulkan currently lack per-Node lowering and must reject a graph with more than one resolved program as `VG_ERROR_UNSUPPORTED`; they must not choose the first module, merge programs, or silently use a host fallback. Vulkan remains compile-review-only under ADR-024/ADR-043.

7. **Raster scope cuts remain in force.** This decision does not enable mixed compute+raster submission. Restricted `vg.msl.raster/v1` remains subject to ADR-047's all-raster limitation and HostAssisted disclosure; SceneRoot remains subject to ADR-052's raster-only and explicit producer/consumer-ordering limits. Multi-CodeObject support is initially a Reference canonical-compute capability, not a backdoor around those decisions.

## Alternatives

- **Keep ADR-044's CodeObject-bound builder as a permanent graph rule:** rejected. It preserves an incomplete capability as authorization, makes NodeRefs depend on an unrelated handle, and cannot express a dependency graph across valid device Nodes.
- **Authorize envelope NodeRefs by index only:** rejected. Slot reuse turns stale authorization into authority for a different generation.
- **Add a public MultiCodeObjectGraph, NodeClass handle, or new function-table entry:** rejected. The existing Device, Node, TaskGraph, Envelope, and complete NodeRef already express the required semantics; a parallel object family would enlarge the ABI without repairing the defect.
- **Pretend Metal/Vulkan support by lowering every task through the first CodeObject:** rejected. It violates the Node contract and hides an Unsupported path. Per-Node backend lowering is a future capability gate.
- **Use this work to allow mixed compute+raster:** rejected. That requires the contract changes and evidence requested by ADR-047/ADR-052.

## Consequences

- Existing one-CodeObject callers remain source and binary compatible: their non-null builder hint is accepted and their single Node resolves as before.
- New callers can pass `NULL` and compose canonical compute Nodes from multiple CodeObjects in one same-device graph on Reference only.
- Destruction of a CodeObject handle no longer invalidates a live Node or an already-resolved submission; destroying the Node itself makes its generation stale for future append/envelope/submit validation.
- Envelope authorization is generation-complete. Backend class vectors cannot be reused as an authority shortcut.
- Backend conformance must cover nullable and non-null same-device builder creation; cross-device hint rejection; multiple CodeObjects with dependency order on Reference; Node and CodeObject destruction/lifetime; stale or recycled NodeRef rejection in graph and envelope; exact generation matching at submit; and explicit Metal/Vulkan Unsupported results. Existing one-CodeObject compute and raster tests remain regression gates.

## Evidence

- `include/vg/vg.h`: `VgNodeRef` is documented as a Device capability token; `VgTaskGraphBuilderDesc.code_object` is nullable compatibility metadata.
- `src/api/vg_api_code.cpp`, `vg_api_taskgraph.cpp`, and `vg_api_execution.cpp`: device-owned NodeTable creation/lookup, immutable CodeObject materialization, resolved-node snapshotting, same-device checks, and complete envelope authorization.
- `src/core/core.h/.cpp`: NodeEntry retains its CodeObject package; NodeTable snapshots under lock and advances generation on destruction.
- `src/backends/reference/reference_device_hal.cpp`: canonical multi-Node compute packages and deterministic execution; `device_hal.h`: resolved-node execution-plan representation. Other backends reject the absent per-Node lowering capability.
- ADR-044 (superseded narrowing), ADR-047, ADR-052, and `docs/vg-project/02-principles-and-semantics.md` §§1, 4–5 establish the controlling authority, lifetime, and raster boundaries.

## Revisit trigger

Revisit when Metal or Vulkan implements verified per-Node lowering; when a single submission needs canonical compute and restricted-MSL raster together; when envelope delegation must authorize a node class rather than named Nodes; or when a future public ABI change genuinely needs a version beyond v1.7. In each case retain complete NodeRef generation checks and report any non-Direct lowering honestly.
