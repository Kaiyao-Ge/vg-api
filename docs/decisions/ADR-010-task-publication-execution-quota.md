# ADR-010: Phase A Task Publication, Schema Binding, and Quota

## Status

Accepted for Phase A PortableCore.

## Decision

`PublicationRing::publish_task` is the checked convenience path for
reserve/write/release-publish. Invalid writes are returned to `Empty` through an
abort transition, and a full ring reports `publication ring quota overflow`.
`TaskGraphBuilder::append_published` acquires a complete published record and
consumes the slot only after the record has been accepted by the builder.

The builder supports task-count and aggregate-payload quotas. Quota failure is
reported before appending and does not mutate the builder. Sealing still produces
an immutable graph; execution validation requires both sealing and publication.
The generated `TaskRoot` schema is converted through `task_from_schema`, keeping
the generated layout as the producer ABI and the checked `TaskRecord` as the
PortableCore representation.

## Consequences

- CPU/GPU publication tests share the same release/acquire state machine.
- Ring exhaustion and task/payload overflow are explicit negative outcomes.
- Generated schema layout remains testable without expanding the public C ABI.
- Continuations, delegated envelopes, and GPU-side execution are later-phase work.

