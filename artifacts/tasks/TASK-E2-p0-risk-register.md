# TASK-E2: P0 风险登记

Status: complete。

Normative docs: `12-roadmap-and-risks.md` §7 P0 表；ADR-042 §6。

## Goal

把七条 P0 风险写成可引用登记：owner、已有测试、范围限制、是否仍开放。
未退休的风险必须有明确范围限制，而不是假装关闭。

## Invariants

- 有测试才能写「有缓解」。
- Task publication 弱内存：仅 host bounded model，无 GPU litmus，标范围限制。
- 不发明尚未存在的 differential / litmus。

## 预计文件

- `docs/reports/p0-risk-register.md` / `.json`

## Tests

文档可解析；`docs.check`。
