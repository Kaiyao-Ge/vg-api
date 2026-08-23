# TASK-D3: 工作集压力与 E011

Status: complete（submit 经 apply_working_set_budget 硬拒绝超预算；发现后租用走 core::discover_reachable，不编假子集）。

Normative docs: `docs/vg-project/02-principles-and-semantics.md` §7.5；
`docs/vg-project/06-backend-macos-metal.md` §4、§10；
`docs/vg-project/07-backend-linux-nvidia-vulkan.md` §13；
`docs/vg-project/09-experiment-catalog.md` E011；`docs/START.md` 不变量 10。

## Goal

证明地址图和「这次驻留多少」是分开的：预算从明显小于 Arena、加到超过预算时，
系统可预测地拒绝或受控降质，而不是把本机统一内存当成无限。

变体：整块 Arena、手选范围、（D2 就绪后）发现后租用。稀疏页：Metal 诚实
Unsupported；Vulkan 只源码对照，不当自动 fault。

可观察结果：同一 workload 在预算内通过、超预算返回明确错误或应用允许的降质；
报告里的工作集字节是请求/承诺/代理三者分开写的。

## Invariants

- 不把缺少公开 counter 写成「没有迁移」（06 §10）。
- 不把 unified memory 当无限，不把 Vulkan sparse 当自动 fault（09 E011）。
- 超预算不得静默夹断成功。
- 逐步加压，有上限和可停条件，不把机器打到不可用。
- FaultManaged 本机仍 Unsupported。

## 预计文件

- `src/core/core.h` / `.cpp`：预算检查（用 D1 类型）；超预算错误文案稳定。
- `src/backends/metal/metal_device_hal.mm`：承诺字节 / heap 代理；能拿到的
  工作集或内存压力代理写入 LoweringReport，拿不到就标 proxy。
- `src/backends/reference/reference_device_hal.cpp`：按字节预算做同一拒绝语义。
- `src/backends/vulkan/vulkan_device_hal.cpp`：sparse 映射说明，compile-review-only。
- `tests/vertical_slice/metal_task_timeline_test.cpp`：`working-set` mode，
  小 Arena 上 10% / 超预算两档即可，不要一上来百万级。
- `experiments/definitions/E011-residency-working-set.json`。

## Tests

- 手选范围 < 预算：过。
- Universe 无预算或预算小于全体 backing：拒，错误可判定。
- 整块 vs 手选：报告字节不同。
- 发现后租用：D2 未合入前标 blocked，不编子集。
- 安全：测试不得申请接近物理内存的分配。

## Depends / Unblocks

Depends: D0、D1。发现后租用可选依赖 D2。
Unblocks: D7 的平台策略表。

## Risks

- M1 上真实驱逐几乎不可观测。必须用代理指标，并在报告里写「proxy」。
- 与 E016 表示水位不是同一件事：E016 管表示版本，E011 管驻留预算。不要复用
  `max_in_flight_representations` 冒充工作集。

## Decision needed

超预算是硬拒绝，还是允许应用显式降质。建议：core 默认硬拒绝；降质只作为
测试夹具里的应用策略（与 E016 drop/quality 同形），不写进 adapter 自动策略。

## Out of scope

实现 Metal/Vulkan sparse 运行时；可恢复 page fault；GPU 时间戳（没有就不要编）。
