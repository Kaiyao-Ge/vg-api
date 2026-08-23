# Phase A Gate Report

状态：`reference-complete`

本报告只判断 `PortableCore` 的 Phase A 退出条件。它不把 CPU/reference
结果升级为 Metal/Vulkan 或原生 driver 合同结论。

| Gate | 结果 | 证据 |
|---|---|---|
| E001 lifecycle/epoch | passed | Arena/GraphEpoch/RepresentationEpoch model test |
| E003 publication | passed | PublicationRing release/acquire, overflow, quota tests |
| E006 witness/diagnostics | passed | reference conformance witness and structured fault fixtures |
| E015 fault/poison | passed | partial-output, stale epoch, certificate fault, capture replay |
| E018 graph publication | passed | immutable GraphEpoch and topology model coverage |
| canonical schema/layout | passed | schema generator and layout golden tests |
| capture/reference replay | passed | normal and fault capture round-trip with re-execution |
| ASan/UBSan | passed | clean sanitized build, `13/13` tests |

## Reproduction

```text
cmake -S . -B /tmp/vg-phase-a-gate -G Ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/vg-phase-a-gate --parallel 2
ctest --test-dir /tmp/vg-phase-a-gate --output-on-failure
python3 tools/vg-exp/vg_exp.py phase-a --build-dir /tmp/vg-phase-a-gate
```

The runner writes an immutable bundle under `artifacts/runs/`, including resolved
definitions, per-experiment samples, logs, summary, and file hashes. The runner
records five deterministic reference gates; it is not a substitute for the larger
performance or native-adapter experiment matrix. The machine-readable gate status
is in [`phase-a-gate.json`](/Users/gokyrie/projects/vg-api/docs/reports/phase-a-gate.json).

## Explicit boundary

Metal/Vulkan lowering, native adapter conformance, dynamic discovery/Universe,
cross-device resource import, and GPU-generated delegated continuation remain
deferred to later phases. No performance claim is made by this gate.
