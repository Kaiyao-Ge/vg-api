# 12 Roadmap, Decisions, and Risks

路线图以风险退休为导向，而不是以代码行或 demo 数量衡量。每一阶段都有入口、产物、退出门槛和停止条件。

## 1. Phase 0：Repository and Evidence Foundation

**目标**：任何 Agent 可构建、测试、产出可追踪 artifact。

**工作**：仓库骨架；CMake presets；C ABI skeleton；logging/result；JSON schema；test runner；experiment runner v0；platform probe；docs/link checks；ADR template。

**退出**：M1 构建 CPU target；Linux server 构建 CPU/Vulkan probe；`vgGetApi` ABI smoke；run bundle 可封存；CI/本地命令有文档。

**不做**：复杂 shader、raster、性能主张。

## 2. Phase A：Portable Semantic Core

**目标**：证明核心代数自洽。

**工作**：schema generator；handles/generation；Arena/Allocation；typed reference；canonical IR/verifier；CPU interpreter；Task builder/seal/publication simulation；effect DAG；Timeline；epochs；certificate；poison；capture v0。

**退出**：E001/E003/E006/E015/E018；random model test；ABI/layout golden；reference capture replay。

**关键决策**：schema、IR、publication、poison、handle。

## 3. Phase B：Compute Adapters

**目标**：同一线性地址图和 Task ABI 在两台真实 GPU 上执行。

**工作**：DeviceHAL plugin；Metal/Vulkan allocation/address；compute codegen/import；root schema；timeline；timestamps；Task Tier0/1；LoweringReport；adapter conformance。

**退出**：E002/E003/E007/E009/E012 两后端结果；无隐藏 host wait；capability rejection 完整。

**Correction (2026-08-21，见 ADR-024)：** 上文"两后端结果"在 Vulkan 真机
永久不可达（本项目开发环境的既定约束）时无法字面满足。ADR-024 记录了本项目
实际采用的关闭标准：Metal+reference 真实硬件结果 + Vulkan 编译审查级证据
（明确标注 `compile-review-only`，从不冒充执行证据），沿用 ADR-021/022 已
确立的先例。这是对本段原文的显式偏离，不是重新解释——原文保留不改写，实际
判定以 ADR-024 及其派生的各实验实现 ADR（ADR-025~029）为准。

**停止检查**：如果 root data 总被迫重建传统 per-task bindings，分析 adapter 与核心设计，不继续堆 feature。

## 4. Phase C：Representation and Raster

**目标**：验证“统一语义 + 专用 facet”对纹理/raster 的可行性。

**工作**：CanonicalView；facet pool/generation；sample/storage/attachment；representation transform；RepresentationEpoch；basic raster/software oracle；pipeline classification；ConsumeInput。

**退出**：E005/E008/E013/E016；图像 correctness；facet stale tests；peak memory report。

**停止检查**：若每次 facet 使用都产生昂贵对象/descriptor update，重新评估 facet ABI/cache，而不是把 texture object 暴露回 public API。

## 5. Phase D：Dynamic Graph and Residency Research

**目标**：定位现有 API adapter 与原生 VG 合同的真实边界。

**工作**：discovery pass；witness；Tier2 lowering；quota continuation；working set pressure；optional sparse；capture visualization。

**退出**：E004/E010/E011/E014/E017；break-even curves；HostAssisted 边界清单；NativeContractResearch v1。

**Correction (2026-08-23，见 [ADR-035](../decisions/ADR-035-phase-d-evidence-policy-and-shared-contracts.md)）：**
证据形状沿用 ADR-024：Metal+reference 真跑，Vulkan 为 compile-review-only，
不冒充执行证据。`HostAssisted`/`Unsupported` 是合法结论。B 阶段 E004
（DiscoverThenLease 退化成 Universe 全扫描）保留为历史结果，不是 D 的发现关门。
租约、预算、信封溢出是独立 core 类型，不是证书字段。原文退出句不改写。

**Correction (2026-08-23，见 [ADR-041](../decisions/ADR-041-phase-d-hostassisted-boundary-and-native-contract-v1.md)）：**
D7 已写出 HostAssisted 边界清单与 NativeContractResearch v1。五门实验有诚实
分类；盈亏曲线样本不足，标未测。这是研究记录，不是产品关门，也不自动开始
Phase E。原文退出句仍不改写。

## 6. Phase E：Research Alpha

**目标**：外部研究者可复现、评价和扩展。

**产物**：versioned C ABI；compiler/tool binaries；Metal/Vulkan adapters；conformance；18 项实验状态；sample workloads；完整 run bundles；architecture paper/report。

**退出**：所有 P0 风险关闭或有明确范围限制；文档与代码一致；另一 Agent 从干净 checkout 可完成 build -> conformance -> 一个 benchmark。

## 7. 优先级风险表

### P0：会否定核心或造成错误

| 风险 | 触发信号 | 缓解/实验 | Owner |
|---|---|---|---|
| Certificate 不 sound | witness 超出声明 | reference instrumentation、E004/E006；拒绝提交 | Core/Compiler |
| Epoch 回收竞态 | stale 引用命中新对象 | generation/model test、延迟回收 | Core |
| Task publication 弱内存错误 | torn payload | litmus、明确 acquire/release | Core/Backend |
| Facet 在途偷换 | 不同 Task 解释不同 | RepresentationEpoch + slot generation | Core/Backend |
| Adapter 弱化同步 | 偶发错误/validation | effect trace/differential/E012 | Backend |
| ABI/schema 分裂 | host/device layout 不同 | single generator + golden | Compiler/API |
| Fault 被当成功 | 部分输出消费 | poison taxonomy/E015 | Core/Backend |

### P1：会让架构不实用或结论失真

| 风险 | 信号 | 缓解 |
|---|---|---|
| Dynamic graph 退化 Universe | 工作集接近 Arena | discovery/bounded allocators/break-even |
| Multi-version 内存峰值 | frames-in-flight 放大 | backpressure/ConsumeInput/E016 |
| Adapter 重建 binding/state | CPU 成本线性增长 | cached facet/root pointer/report/E007 |
| Pipeline explosion | compile hitch/cache 巨大 | StateBlock classification/E013 |
| Tier2 普遍 host-assisted | GPU autonomy 中断 | bucket pass/continuation；记录 native gap |
| Metal/Vulkan 语义交集过小 | core 被最低公分母拖累 | capability profiles + optional contracts |
| Benchmark 不公平 | VG 只赢旧式 baseline | idiomatic modern baselines、审计 |
| 可观测性开销高 | checked profile 不可用 | sampling/full reference/fast profile |

### P2：工程与研究效率

| 风险 | 缓解 |
|---|---|
| 自研语言吞噬项目 | IR/schema first，前端延后 |
| 两机器环境漂移 | environment manifest、pinned presets |
| 服务器不可持续可用 | CPU/Metal 日常，Vulkan batch runs |
| 文档与实现漂移 | normative link/test + task workflow |
| backend 代码污染 core | build target/header dependency checks |
| 结果数据过大 | content-addressed archive、保留策略 |

## 8. 架构级未决问题

1. **地址身份**：portable capture 以 allocation+offset 还是 relocatable object graph 为主？Phase A ADR。
2. **Region shape/layout IR**：静态泛型与动态 descriptor 的边界？用 E002/E008 约束。
3. **Certificate composition**：跨间接调用/Task child 如何合并且不爆炸？Phase D。
4. **Facet capability 宽度**：32-bit index+generation 是否足够？通过 limits/fuzz。
5. **Task ABI 大小**：固定 header 与 payload indirection 的平衡？E003/E009。
6. **Node specialization**：哪些 state 进入 key？E013。
7. **Poison 粒度**：submission、Region range 还是 Task？先保守 submission/write-set，再实验。
8. **Discovery publication**：GPU-side set 能否不经 host 直接供 adapter residency？现有 API 很可能限制；保留研究结论。
9. **Cross-backend sample semantics**：过滤/精度差异的 portable contract？通过格式能力表和 tolerance。
10. **Envelope continuation**：portable overflow queue 或预授权 delegated envelope？Phase D。

## 9. 停止/转向条件

项目不因某一 benchmark 失败立即停止，但出现以下情况必须冻结 feature 并复核：

- core semantics 无法给出可实现的 sound certificate；
- 统一 Region 迫使线性访问或采样持续大幅慢于直接 backend 且无法通过缓存消除；
- effect 推导只能普遍产生全局 barrier；
- Task ABI 在 Metal/Vulkan 都只能逐 Task host-assisted；
- RepresentationEpoch 在正常 workload 无法通过 backpressure 控制峰值；
- validation/capture 不能定位裸 pointer fault；
- 公共语义开始充满以 Metal/Vulkan 名称命名的例外。

转向可包括：收窄 profile、把能力降为 research extension、改变 IR/contract、只保留成功的 compute core。禁止通过隐藏成本宣布成功。

## 10. 决策治理

每个 milestone review 输出：capability matrix、conformance、benchmark evidence、risk changes、accepted/superseded ADR、下一阶段预算。决定进入下一阶段前，所有 P0 有 owner/test；未决 P1 有 experiment；不存在只有口头假设的核心 ABI。

## 11. 建议首个 12 周次序

| 周 | 结果 |
|---|---|
| 1 | repository/build/test/docs checks/platform probe |
| 2 | C ABI skeleton、handles/result/loader |
| 3 | schema generator + layout golden |
| 4 | Arena/allocation/epoch reference model |
| 5 | Task builder/publication/effect model |
| 6 | canonical IR verifier + CPU interpreter |
| 7 | DeviceHAL + Metal compute root pointer |
| 8 | Vulkan compute BDA root pointer |
| 9 | Timeline + conformance + LoweringReport |
| 10 | E002/E003/E007 initial runs |
| 11 | SampleFacet minimal Metal/Vulkan |
| 12 | review risks/ADR/data，决定 Phase C |

这不是时间承诺；每周以退出证据为准，不以强行赶进度跳过模型测试。

