# ADR-008: Phase A Effect DAG and Timeline Happens-Before

## Status

Accepted for Phase A PortableCore.

## Decision

`EffectGraph` stores immutable directed edges with a reason: explicit dependency,
inferred conflict, Timeline dependency, or publication. Two effects conflict only
when they refer to the same allocation and representation epoch, overlap in byte
range, and are not both `Read`. This keeps overlapping read/read tasks parallel
while ordering write/read, write/write, atomic, and publish interactions.

`TaskGraphBuilder::seal` adds inferred edges in task order, validates the resulting
DAG, and rejects any conflicting effects that lack a reachable happens-before edge.
The sealed `TaskGraph` exposes the reason-bearing graph for diagnostics and future
adapter lowering; no backend barrier or global synchronization is introduced here.

Timeline signals remain strictly increasing. A Timeline edge records its required
point and is rejected when the point is zero or has not been signaled. Unsatisfied
waits are reported explicitly and do not mutate Timeline state.

## Consequences

- PortableCore can diagnose missing ordering and distinguish inferred hazards from
  user dependencies.
- Independent read-only branches remain eligible for parallel execution.
- Timeline validation is deterministic and backend-neutral.
- Backend-specific barrier merging and queue/encoder lowering remain Phase B.

