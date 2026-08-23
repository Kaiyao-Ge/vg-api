# ADR-002: Phase 0 run bundle format

Status: Accepted

## Context

Phase 0 requires evidence that can be inspected and archived without adding a large
runtime dependency.

## Decision

`vg-exp` emits JSON/JSONL bundles with `manifest.json`, environment/build identity,
resolved definition, logs, samples, summaries, and SHA-256 hashes. The manifest does
not hash itself. Machine names are redacted and a salted-free machine hash is used
only for same-machine grouping.

## Alternatives

An external database, Python package-based schema validator, or an opaque archive.

## Consequences

Phase 0 schemas are versioned JSON documents. Validation is intentionally structural;
full JSON Schema evaluation can be added when a controlled dependency is justified.

## Evidence

`tests/tools/test_schemas.py` validates a known good definition and rejects a missing
required field.

## Revisit trigger

Before capture integration, benchmark aggregation, or cross-machine comparison.
