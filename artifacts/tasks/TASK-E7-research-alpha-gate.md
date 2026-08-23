# TASK-E7: Research Alpha 记录

Status: complete。

Normative docs: 12 §6 原文退出句；ADR-042。

## Goal

把 `phase-e-gate` 标成 `gate-recorded-per-adr-042`（研究记录，不是产品关门）。
对照退出句：P0 关闭或有范围限制；文档与代码一致；另一 Agent 能走完 runbook。
compiler/tool binaries 仅源码目标、无发行包装，写范围限制。
12 §6 原文不改写。

## Invariants

- 不得把研究记录写成产品关门。
- 不得把未测曲线补点。
- D 的 `phase_e: not-started` 改为指向本记录。

## 预计文件

- `docs/reports/phase-e-gate.md` / `.json` 状态字段
- `docs/reports/phase-d-gate.json` 的 `phase_e` 字段
- `docs/vg-project/12-roadmap-and-risks.md` §6 Correction 旁注

## Tests

gate JSON 可解析；与 E1 runner 的 18 项 id 一致。
