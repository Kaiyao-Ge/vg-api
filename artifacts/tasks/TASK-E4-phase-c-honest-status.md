# TASK-E4: Phase C 诚实状态

Status: complete。

Normative docs: ADR-030；`docs/reports/phase-c-gate.md`（`not-closed`）；
ADR-042 §3。

## Goal

注册已存在的 `tooling.phase-c-runner`。用已跑过的 E005/E008/E016
（及非硬门槛 E013）补一行诚实记录。Phase C 整体保持 `not-closed`，
除非现有证据已经满足 ADR-030 书面退出（含 raster oracle 与峰值字节）。
当前不满足，因此不写成产品关门。

## Invariants

- 不把 layer1 改写成完整 Phase C 退出。
- 峰值字节 / hitch 仍为 unmeasured。

## 预计文件

- `cmake/phase-e-e4.cmake`（注册 phase-c runner）
- `docs/reports/phase-c-gate.md` / `.json` 增补实验行，状态仍 `not-closed`

## Tests

`tooling.phase-c-runner`（`VG_ENABLE_METAL`）。
