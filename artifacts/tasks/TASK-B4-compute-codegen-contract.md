# TASK-B4: Shared Compute Codegen Contract

Status: complete

Normative docs: `docs/START.md`; `docs/vg-project/02-principles-and-semantics.md`; `docs/vg-project/13-repository-layout.md` §8

## Goal

Finalize the target-neutral `ComputePackage` contract, the B4 linear
subset definition, and a golden-fixture mechanism that B5 (Metal) and B6
(Vulkan) can both build against without duplicating semantic expectations.

## Invariants

- Linear subset is exactly `load`/`store` (4-byte, 4-byte-aligned) and
  `atomic_add` (8-byte, 8-byte-aligned, matching the reference executor's
  `int64_t` atomic contract). `publish` and any other op is rejected, not
  silently dropped.
- Every generated MSL/GLSL line traces back to an IR instruction via
  `ComputeSourceMapEntry`.
- A 64-bit atomic that cannot be lowered natively is reported through
  `LoweringReport`, never silently truncated to 32 bits.
- Golden fixtures and their generated-source snapshots are committed,
  human-reviewable artifacts; `tools/vg-golden-gen` is never invoked from CI.

## Files

- `src/compiler/compute_package.cpp`, `src/compiler/compute_package.h`
- `src/backends/device_hal.h` (`LoweringEvent`/`LoweringReport`)
- `tests/fixtures/ir/{load_only,store_only,atomic_add_only,mixed}.vgir.json`
- `tests/fixtures/golden/<name>.{msl,glsl,sourcemap}.golden`
- `tools/vg-golden-gen`
- `tests/unit/compute_package_test.cpp`, `compute_package_golden_test.cpp` (CTest `compiler.compute-package`, `compiler.compute-package-golden`)
- `docs/decisions/ADR-015-b4-compute-codegen-contract.md`

## Validation

`compiler.compute-package` and `compiler.compute-package-golden` pass
locally (verified under `dev-metal` preset, part of the full 18/18 CTest
run alongside B5/B4b work).

## Updating golden fixtures

Golden files are committed, human-reviewable artifacts (`docs/vg-project/13-repository-layout.md`
line 161: "the command to update golden files and the diff must live in
the task note"). To regenerate them after a deliberate codegen change:

```sh
cmake --build --preset dev-metal --target vg-golden-gen
./build/dev-metal/vg-golden-gen "$(pwd)"
git diff -- tests/fixtures/golden/
```

Review the diff before committing. `vg-golden-gen` is never invoked from
CI or CTest; `compiler.compute-package-golden` only *checks* the committed
golden files against a fresh in-memory build, it never rewrites them.

## Known limits

Codegen targets only the linear subset; task-graph/publication lowering
remains B7/B8. Golden snapshots cover four minimal fixtures, not
exhaustive instruction combinations.
