# Phase B Gate Report

状态：`gate-closed-per-adr-024`

本报告判断路线图（`docs/vg-project/12-roadmap-and-risks.md` Phase B 段）
对 `ComputeAdapter` 定义的退出门槛。B7/B8 的机制层交付（Task
Tier0/timeline/Tier0+Tier1）与 `09-experiment-catalog.md` 门槛表要求的
E002/E004/E007/E009/E012 五个实验结果，是两件分开验证的事，现在**两者都已
完成**：

- **B7/B8 milestone（已完成）**：DeviceHAL 的 Task Tier0/timeline/Tier0+Tier1
  机制在 reference/Metal 上真实实现并硬件验证，Vulkan 编译审查级。见
  TASK-B7、TASK-B8、ADR-019~022。
- **路线图 Phase B 退出门槛（已按 ADR-024 关闭）**：五个实验现在都有真实
  Metal+reference 硬件结果，外加 Vulkan 编译审查级证据。

| Gate | 结果 | ADR | Task | ctest |
|---|---|---|---|---|
| E002 typed pointer graph | passed | ADR-028 | TASK-B15 | `vertical-slice.metal.pointer-graph`, `compiler.compute-package-golden` |
| E004 AccessCertificate 模式 | passed | ADR-025 | TASK-B11 | `vertical-slice.metal.access-certificate` |
| E007 root pointer vs binding | passed | ADR-029 | TASK-B16 | `vertical-slice.metal.indexed-binding` |
| E009 GPU-generated same-Node work | passed | ADR-026 | TASK-B13 | `vertical-slice.metal.tier1-indirect`, `vertical-slice.metal.cull-compact` |
| E012 Effect DAG/timeline sync quality | passed | ADR-027 | TASK-B14 | `vertical-slice.metal.effect-dag` |

每一行的"passed"专指 Metal（真实 Apple Silicon 硬件）+ reference 结果；
Vulkan 对全部五行都是编译审查级（`vg_backend_vulkan` 在本机不是构建目标，
无 ctest 执行），由 `tools/vg-exp/vg_exp.py phase-b` 固定标注为
`compile-review-only`，从不与"passed"混淆。机器可读状态见
[`phase-b-gate.json`](/Users/gokyrie/projects/vg-api/docs/reports/phase-b-gate.json)。

## 机制层证据

- Reference：`task_graph`/`timeline_wait`/`timeline_signal` 真实消费，字节级
  oracle（ADR-019）。
- Metal：Tier0 + timeline 硬件验证，`dev-metal` preset，`ctest
  --output-on-failure`：全部 27 个测试通过（含新增的
  `tooling.phase-b-runner`，2026-08-21）。
- Vulkan：Tier0 + Tier1 + timeline 编译审查级，无 Linux/NVIDIA 执行证据。

## 五个实验的真实工程实现（TASK-B10~B17）

每个实验都由一份独立 ADR、一个 `artifacts/tasks/TASK-B*.md` 任务闭环文档、
一份 `experiments/definitions/E*.json` 实验定义、以及至少一个在 `dev-metal`
下真实硬件通过的 ctest 支撑：

- **E004**（ADR-025，TASK-B11）：`CertifiedPinned`/`Universe` 在 Metal/
  reference 上均为 `Direct`；`DiscoverThenLease` 诚实报告为
  `HostAssisted`（统一内存架构下没有真正可发现的 GPU 驻留状态，这是预期
  结果，不是缺口）；`SoftwarePaged`/`FaultManaged` 在两个后端均诚实报告
  `Unsupported`，两份后端设计文档已预先授权这个结果。
- **E009**（ADR-026，TASK-B13）：Metal 真实 `dispatchThreadgroupsWithIndirectBuffer:`
  Tier1 间接派发 + 原子计数器 stream-compaction cull/compact kernel，正式
  取代 ADR-021 的"Tier1 推迟"表述；Vulkan 编译审查级复用 ADR-022 已实现的
  `dispatch_task_ring_and_tier1`。
- **E012**（ADR-027，TASK-B14）：通用 `EffectGraphBuilder` + 按图形状分支
  的 Metal encoder/fence lowering，覆盖 4 种形状中的 3 种（linear chain、
  independent branches、fork-join）；cross-queue/representation-transition/
  external-present 诚实标注 `Unsupported`/`Deferred`。
- **E002**（ADR-028，TASK-B15）：新增 `load_ref`/`load_via`/`store_via`
  opcode + `PointerEdge`/`declared_pointer_edges`，Metal/reference 均为
  `CachedObject` lowering（目标已在 host 侧静态解析，真实设备指针解引用
  对此里程碑范围而言不成比例）。
- **E007**（ADR-029，TASK-B16）：新增 `IndexedComputeBinding`/
  `build_indexed_compute_package`，把 N 个独立分配折叠成 1 个
  argument-buffer 风格表绑定，Metal 上是真实的 `Direct` 设备指针表解引用
  （与 E002 的 `CachedObject` 选择相反，理由见 ADR-029）；本机 Metal 设备
  确认真实支持 `MTLBuffer.gpuAddress`，因此该结果是真实硬件执行，不仅是
  诚实降级记录。Vulkan 侧的真正描述符索引 bindless 基线明确推迟——本计划
  最大的一处范围推迟。

## 关闭标准（ADR-024）

Phase B 的实际关闭标准以
[ADR-024](/Users/gokyrie/projects/vg-api/docs/decisions/ADR-024-phase-b-gate-closure-metal-reference-vulkan-compile-review.md)
为准：Metal+reference 真实硬件结果 + Vulkan 编译审查级证据（明确标注
`compile-review-only`），而非路线图原文字面的"两后端结果"——这是 Vulkan
真机永久不可达约束下的显式记录在案的偏离，沿用 ADR-021/022 先例。
**该标准现已满足**：上表五行全部为真实结果，TASK-B10~B17 全部完成。

## 复现

`python3 tools/vg-exp/vg_exp.py phase-b --build-dir build/dev-metal`
（或 `ctest -R tooling.phase-b-runner --test-dir build/dev-metal`）产出一次
`vg.summary/v1` 运行，聚合五个实验各自的 ctest 结果，Vulkan 固定标注为
`compile-review-only` 的独立样本，从不计入 pass/fail。

## 明确边界

**本报告不冒充 Vulkan 有任何真实执行证据** —— 全部五行的 Vulkan 部分永远
是编译审查级（`vg_backend_vulkan` 在本机不是构建目标，Vulkan 真机永久
不可达，ADR-024）。**已知的诚实推迟/未覆盖范围**（均记录在各自 ADR 中，
非遗漏）：E004 的 `SoftwarePaged`/`FaultManaged`；E009 的 GPU 硬件时间戳；
E012 的 cross-queue/representation-transition/external-present 三种图形状；
E002 的多跳指针链；E007 的真实 Vulkan 描述符索引 bindless 基线。
