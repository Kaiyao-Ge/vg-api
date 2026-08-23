# TASK-D4: Tier2 多节点选择与 E010

Status: ready（不依赖 D1–D3；可与发现/驻留/抓包并行）。

Normative docs: `docs/vg-project/06-backend-macos-metal.md` §8；
`docs/vg-project/07-backend-linux-nvidia-vulkan.md` §10；
`docs/vg-project/09-experiment-catalog.md` E010；
ADR-021 / ADR-026（ICB 属于 Tier2，不是 Tier1）。

## Goal

在预授权的多种 Node / 管线里，让 GPU 选择接下来跑哪一种。本机若间接命令缓冲
可用且命令类型够用，走预编码选择；否则用分桶 compute + 每节点 indirect，
分类为 `EmulatedDevicePass`，记下分桶、临时字节、切管线成本。

不要求 VG 快过手写 baseline。必须标清：哪些是设备遍，哪些是主机协助。
Tier3（GPU 自己扩信封、跨域乱建）继续 Unsupported。

可观察结果：2 种以上预授权 Node，GPU 侧选出的集合与 CPU 裁判一致；
报告里能看出 ICB 或分桶，而不是把「读回计数再编码」标成 GPU-driven。

## Invariants

- 不得把 CPU 读计数后重新编码标成 native GPU-driven（06 §8）。
- 预编码 / inherit / reset / optimize 成本必须进 LoweringReport。
- 能力不够就降级或 Unsupported，不静默夹成 Tier1 同节点。
- 选择必须落在信封已授权的 Node 集内；不能伪造新 Node。
- Vulkan 只 compile-review-only。
- 与 D5 的「爆配额」衔接是后续，本任务先保证选择正确、不超静态 quota。

## 预计文件

- `src/backends/metal/metal_device_hal.mm`：ICB 能力探测；预编码多 Node 或
  分桶 + per-Node indirect。
- `src/backends/reference/reference_device_hal.cpp`：按图选择 Node 的字节级裁判。
- `src/backends/vulkan/vulkan_device_hal.cpp`：DGC 或分桶路径的源码对照。
- `src/compiler/`：仅当预授权 Node 集必须进 package 时才加元数据，避免新 IR 方言。
- `tests/vertical_slice/metal_task_timeline_test.cpp`：`tier2-nodes` mode。
- `experiments/definitions/E010-heterogeneous-node-lowering.json`。
- ADR：记录本机实际走了 ICB 还是分桶。

## Tests

- 2 个 Node class、均匀与偏斜各一档（小数量，不要 256 一上来）。
- 选出的 Node 集合 == 参考裁判。
- 故意选未授权 Node → 拒。
- ICB 不可用时：分桶路径过，且 report 不是 DevicePass 冒充。
- 不要求完整 CPU/GPU 成本曲线；D7 汇总。本任务至少报告 bucket/command 计数。

## Depends / Unblocks

Depends: Tier0/1（已有）；D0 证据政策。不依赖租约/发现。
Unblocks: D5 若要演示「选择后任务数爆配额」；D7 成本曲线。

## Risks

- M1 ICB 命令类型可能不够。默认设计成分桶可运行，ICB 是能力升级，不是硬门槛。
- 不要为 E010 发明新的公共「pipeline object」API。

## Decision needed

第一刀是否强制 ICB。建议：不强制；有则试，无则分桶 + 诚实分类。

## Out of scope

Tier3；跨 queue 异构；网格/光线节点；256 类节点的正式性能矩阵。
