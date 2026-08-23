# TASK-D7: HostAssisted 边界清单与 NativeContractResearch v1

Status: complete（五门有诚实分类；曲线未测；清单 + v1 已写出；未开 Phase E）。

Normative docs: `docs/vg-project/12-roadmap-and-risks.md` §5 退出、§8 未决 3/8/10、
§3 第三轨道 NativeContractResearch；`docs/START.md` §3；
`docs/vg-project/01-project-charter.md` §5.4（负面结果也算成功）；
ADR-035（D0）；ADR-041（本任务）。

## Goal

Phase D 的退出不是再写一个功能，而是把适配器真实边界写成两份可引用产物：

1. **HostAssisted 边界清单**：发现是否必须回主机、多节点是否必须分桶、
   续跑是否必须第二次提交、工作集是否只有 proxy、抓包跨后端是否只是语义对照。
2. **NativeContractResearch v1**：哪些能力只有 UMD/KMD/firmware 合同才可能
   去掉上述协助；明确本机不会实现。这是研究轨道，不是驱动开工单。

同时汇总 E004/E010/E011/E014/E017 的 Metal+reference 结果与 Vulkan
compile-review-only 行，以及能画的盈亏/成本曲线（没有测量就写未测，不编点）。

可观察结果：另一人只读这两份产物 + 实验定义，能说出「VG 在现有 API 上
哪里停住、原生合同该从哪问」。

## Invariants

- 不得把 HostAssisted 改写成 Direct 来关门。
- 不得把未跑的 Vulkan 填进执行证据。
- 不得把 v1 写成「下一步实现 KMD」。
- 不改写 12 路线图原文；用 ADR / reports 关门。
- 曲线没有测量就标未测或样本不足。

## 预计文件

- `docs/reports/phase-d-gate.md` / `.json`
- `docs/reports/host-assisted-boundary.md`
- `docs/reports/native-contract-research-v1.md`
- `tools/vg-exp/vg_exp.py`：`phase-d` 映射五门 ctest + Vulkan compile-review-only 行
- 各 `experiments/definitions/E00{4,10,11,14,17}*.json` judgement 回填真实分类
- `docs/decisions/ADR-041-phase-d-hostassisted-boundary-and-native-contract-v1.md`

## Tests

`python3 tools/vg-exp/vg_exp.py phase-d --build-dir build/dev-metal` 五门
Metal/reference 行有结果或明确 Unsupported；gate json 可解析。无新 GPU 功能。

## Depends / Unblocks

Depends: D2–D6 均有可引用结果（允许单门 Unsupported，不允许「没做」）。
Unblocks: Phase E 入口（对外复现），不自动开始 E。

## Implementation (2026-08-23)

五问答案见边界清单：发现必须回主机；多 Node 必须分桶（EmulatedDevicePass）；
续跑必须第二次提交；工作集只有 proxy；跨后端抓包只是语义对照。
盈亏曲线标 unmeasured。`phase-d` 只加载 `E004-discovery-revisit.json`。
Phase E 未开。
