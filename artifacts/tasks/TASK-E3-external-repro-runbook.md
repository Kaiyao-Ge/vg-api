# TASK-E3: 对外复现路径

Status: complete。

Normative docs: ADR-042；12 §6 退出句「另一 Agent 从干净 checkout
可完成 build → conformance → 一个 benchmark」；`10` §15。

## Goal

更新执行入口的事实状态，并写一份单一 runbook。不改写 vg-project 原文。

## Invariants

- `docs/START.md` 是执行入口，可以更正过时的「尚未建立 CMake」句子。
- README Status 必须覆盖 A–D 已记录、E 进行中/已记录。
- runbook 不要求第二台 Vulkan 真机。

## 预计文件

- `README.md`、`docs/START.md` 事实状态
- `docs/reports/external-repro-runbook.md`

## Tests

`docs.check`。命令与 preset 名称必须真实存在。
