# TASK-012: Phase A Gate Report and Handoff

状态：已完成（Reference gate）

## 交付内容

- 新增 [`docs/reports/phase-a-gate.md`](../../docs/reports/phase-a-gate.md) 与机器可读
  [`phase-a-gate.json`](../../docs/reports/phase-a-gate.json)。
- 汇总 E001/E003/E006/E015/E018、schema/layout、capture replay 和 ASan/UBSan 证据。
- 当前 clean build 的 CTest 门禁为 `13/13`，不是早期文档中的 `12/12`。
- 明确 PortableCore 已通过、NativeAdapter 与动态 residency 仍 deferred 的边界。
- 给出从干净构建到 CTest、Phase A runner 的复现命令。

## Gate 结论

Phase A PortableCore 达到 reference-complete；Phase B 入口条件是保留本报告的
reference oracle，并为 Metal/Vulkan adapter 复用同一 canonical definitions 和
negative fixtures。当前报告不包含性能胜负结论。
