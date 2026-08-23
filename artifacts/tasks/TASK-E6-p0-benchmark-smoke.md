# TASK-E6: P0 benchmark 入口

Status: complete。

Normative docs: `10-validation-and-benchmarks.md` §13 P0；ADR-042 §5。

## Goal

`vg-exp benchmark` 包装一个已有 Metal vertical slice
（`vertical-slice.metal.tier2-nodes`）。输出 run bundle；
`evidence_grade: P0`；只记 host wall-clock。不标 `gpu_ns`。

## Invariants

- 不发明 hitch / break-even。
- 未测项继续写 `unmeasured`。
- 失败不得改成跳过。

## 预计文件

- `tools/vg-exp/vg_exp.py`：`benchmark`
- `cmake/phase-e-e6.cmake`
- `tests/tools/test_phase_e_benchmark.py`

## Tests

`tooling.phase-e-benchmark`（`VG_ENABLE_METAL`）。
