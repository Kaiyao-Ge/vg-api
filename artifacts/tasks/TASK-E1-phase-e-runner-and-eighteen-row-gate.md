# TASK-E1: phase-e runner 与 E001–E018 状态表

Status: complete。

Normative docs: ADR-042；`09-experiment-catalog.md` Research Alpha 门槛；
A/B/C/D gate 报告。

## Goal

`vg-exp phase-e` 聚合已有 ctest，写出 18 行状态。E004 只加载
`E004-discovery-revisit.json`。每项追加一条 Vulkan compile-review-only
样本，不与 `passed` 混计。

可观察结果：`tooling.phase-e-runner` 在 `dev-metal` 上 18 项定义齐全，
执行行全过或标 `missing`，Vulkan 18 行均为 compile-review-only。

## Invariants

- 不新写 GPU kernel。
- 不改写 B 时代 `E004-access-certificate.json`。
- 分类只引用已有 judgement / gate。

## 预计文件

- `tools/vg-exp/vg_exp.py`：`phase-e`
- `docs/reports/phase-e-gate.md` / `.json`
- `cmake/phase-e-e1.cmake`
- `tests/tools/test_phase_e_runner.py`

## Tests

`tooling.phase-e-runner`。需要 `VG_ENABLE_METAL`。
