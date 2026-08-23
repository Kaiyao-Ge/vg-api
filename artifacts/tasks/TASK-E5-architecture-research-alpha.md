# TASK-E5: 架构报告骨架

Status: complete。

Normative docs: `01-project-charter.md` G1–G6；A–D gate；
`host-assisted-boundary.md`；`native-contract-research-v1.md`；ADR-042。

## Goal

写一份可引用的架构研究骨架，只陈述已记录分类，不写新性能主张。

## Invariants

- 证据等级不得超过已有 gate。
- 不把 Metal ICB `DevicePass` 写成原生驱动。
- 不把 HostAssisted 改写成 Direct。

## 预计文件

- `docs/reports/architecture-research-alpha.md`

## Tests

`docs.check`。
