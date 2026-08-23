# TASK-D5: 信封配额续跑与 E017

Status: complete（E017 portable continuation = overflow buffer + next submit；ADR-039）。

Normative docs: `docs/vg-project/02-principles-and-semantics.md` §5.3、§11；
`docs/vg-project/09-experiment-catalog.md` E017；
`docs/vg-project/12-roadmap-and-risks.md` 未决问题 10；
ADR-010（现有静态 quota，不是续跑）。

## Goal

高度动态的 Task 展开在碰到配额时，要有可移植的续法：溢出进缓冲，下一提交
接着干；或主机协助把信封扩一截。第一阶段选出一种 portable continuation，
写进报告。固件微内核只许模拟研究，不许写成可部署。

现有 `TaskGraphBuilder::set_quota` 只在建图时拒绝。本任务处理的是：
提交已经开始、GPU 还想再发布时，装不下怎么办。

可观察结果：静态大配额仍一次过；故意把 quota 压到会溢出时，第一次提交
带着溢出记录成功或明确部分完成，第二次提交吃掉溢出且不丢任务、不双发；
错误路径不会把部分输出标成成功。

## Invariants

- 越过信封需要新 commit 或预授权 DelegatedEnvelope（02 §5.3）。本机没有
  真 DelegatedEnvelope 硬件时，不得假装有。
- 自动 retry 仍受 02 §9 限制（幂等 / 独立目的 epoch / 有重建配方）。
- 不得静默把 quota 加大后报成功。
- 固件扩信封 = 模拟研究，lowering class 不得写成 DevicePass。
- 与 D4 的交汇（GPU 选 Node 后数量爆了）可以后接，本任务先用 CPU/Tier0
  发布撑满 quota 即可证明续跑。

## 预计文件

- `src/core/core.h` / `.cpp`：消费 D1 的溢出记录；「下一提交」输入输出。
- `src/backends/device_hal.h`：ExecutionPlan / Submission 上的 optional 续跑字段。
- `src/backends/metal/metal_device_hal.mm` 与 reference：第二次 submit 接上。
- `src/backends/vulkan/vulkan_device_hal.cpp`：compile-review-only。
- `tests/unit/core_test.cpp`：溢出记账、不可把拒收写成已续。
- `tests/vertical_slice/metal_task_timeline_test.cpp`：`envelope-continuation`。
- `experiments/definitions/E017-envelope-quota-continuation.json`。
- ADR：选出的 portable 机制（建议：overflow buffer + 下一提交）。

## Tests

- 大静态 quota：一次提交，无溢出。
- 小 quota + 故意多发布：第一次留下溢出计数；第二次吃完；任务集合与裁判一致。
- 第二次提交不带续跑令牌：不得偷偷吃掉上一次溢出。
- 主机协助扩信封：若做，必须标 HostAssisted。
- 不要求 work stealing / 递归细分的完整性能矩阵。

## Depends / Unblocks

Depends: D0、D1。可选后接 D4。
Unblocks: D7 的 continuation 选择说明。

## Risks

- 和 PublicationRing 现有 overflow 文案撞车。环满是「这一环写不下」；
  信封溢出是「这一提交的授权用完」。两种错误必须能区分。
- 不要把续跑做成隐式全局队列，破坏信封可审核性。

## Decision needed

第一刀选哪一种 portable 机制。建议：overflow buffer + 下一提交。
大静态 quota 作为对照，不作为唯一路径。

## Out of scope

真 DelegatedEnvelope；KMD/firmware；跨进程续跑；公平性大规模 benchmark。

## Implementation (2026-08-23)

Portable 选择：overflow buffer + 下一提交（ADR-039）。Host 按
`TaskGraph::deterministic_order` 切图，标 `HostAssisted`。令牌由
`core::EnvelopeContinuationTable` 记在 `DeviceHal` 上。Reference `submit()`
已挂钩。Metal `submit()` 已接 `apply_envelope_continuation`：未设 quota
仍走原 GPU 全图发布；配额切图或 leftover drain 只发布 `order`，标
`HostAssisted`，不把停住的后缀送进 task ring。
