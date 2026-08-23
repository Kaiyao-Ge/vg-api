# TASK-D2: 真正的发现扫描与 E004 重访

Status: complete（可达集 < Universe；HostAssisted 主机走图；B 时代 E004 定义未改写）。

Normative docs: `docs/vg-project/02-principles-and-semantics.md` §7.2、§11；
`docs/vg-project/06-backend-macos-metal.md` §10；
`docs/vg-project/09-experiment-catalog.md` E004；
ADR-025 及 Revisit；E002 指针图（已有）。

## Goal

让 `DiscoverThenLease` 先跑一遍无副作用发现，从已驻留种子图走出**实际够得着、
且能小于整块 Arena** 的集合，再压成证书和租约。证书必须盖住见证。
走完还要回主机再提交，就如实标 `HostAssisted`。

这是对 B 阶段 E004 的重访，不是重跑「主机全扫、结果等于 Universe」。

可观察结果：同一 Arena 里，种子只达一部分节点时，发现集大小 < Universe；
CertifiedPinned 仍只覆盖静态声明；见证超出证书则拒绝提交。

## Invariants

- GraphEpoch 和影响地址选择的输入在发现期间冻结（02 §7.2）。
- 发现 Node 无副作用，不能写业务数据。
- 不得编造「最近碰过」的假子集来画盈亏曲线。
- 不得把主机全扫再标成 GPU discovery。
- `SoftwarePaged` / `FaultManaged` 在本机仍诚实 Unsupported，除非硬件真有合同。
- Vulkan 只 compile-review-only。

## 预计文件

- `src/core/core.cpp`：发现见证 vs 证书的覆盖检查；租约写入发现集。
- `src/backends/metal/metal_device_hal.mm`：发现遍（compute 走指针图 + 回收可达集）。
  若必须 readback 再租，LoweringReport 标 HostAssisted，并记下 discovery 耗时与字节。
- `src/backends/reference/reference_device_hal.cpp`：CPU 上同一发现语义，当裁判。
- `src/backends/vulkan/vulkan_device_hal.cpp`：源码对照，不执行。
- `tests/vertical_slice/metal_task_timeline_test.cpp`：新 mode，例如
  `access-certificate-discovery`。
- `experiments/definitions/E004-access-certificate.json`：重访 judgement /
  新 definition id，保留 B 的退化结论不被覆盖。
- `tools/vg-exp/vg_exp.py`：Phase D runner 映射（可与 D7 一起挂）。

## Tests

- 参考实现：可达 1%–若干档的小图，发现集严格小于 Universe，且被证书覆盖。
- Metal：同一图真跑；报告 discovery 时间、扫描/结果字节、是否 HostAssisted。
- 负例：见证超出证书 → 拒绝；发现中途改 GraphEpoch → 拒绝。
- 不要求本任务画出完整 1%–100% 正式曲线；曲线门槛在 D7 汇总。本任务至少两档
  可达比例，证明集合真的能分开。

## Depends / Unblocks

Depends: D0、D1（租约类型）；现有 E002 指针图。
Unblocks: D3 的「发现后租用」变体；D6 抓动态图；D7 盈亏曲线汇总。

## Risks

- 本机统一内存上，「驻留子集」仍可能只是软件集合。必须写明这是语义可达集 /
  代理工作集，不能写成 OS 页迁移已发生。
- 与 D3 抢 `ExecutionPlan` 字段：只通过 D1 的租约/预算，不在这里再加一套 mode。

## Decision needed

发现结果回到 host 再 `submit` 第二次，是本机默认路径（HostAssisted），
还是尝试 GPU 侧 compact 后同提交续跑。建议：第一刀 HostAssisted 两次提交，
同提交续跑列为可选，成功了再升级分类。

## Out of scope

工作集超预算驱逐（D3）；Tier2（D4）；抓包可视化（D6）；firmware discovery。

## Implementation (2026-08-23)

`discover_reachable` + `run_discovery_stage` 已落地。Metal
`attach_access_certificate` 在 `discovery_seeds` 非空时跳过 B 时代全扫，
避免把发现子集盖成 Universe。`vg-exp` 的 Phase D 映射留给 D7。
