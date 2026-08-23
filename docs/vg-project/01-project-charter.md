# 01 Project Charter

## 1. 项目身份

VG 是一个研究型、可运行、可测量的 GPU API/编译器/运行时项目。它不是 Vulkan 的 wrapper，不是 CUDA 的跨平台别名，也不是试图在用户态绕过厂商驱动的黑客接口。

当前阶段的产品是三件事：

1. 一套比传统资源对象/绑定/逐资源状态机更接近 GPU 数据平面的公共语义。
2. 一套能把该语义降低到 CPU reference、Metal 和 Vulkan 的编译器/运行时架构。
3. 一组实验与报告，诚实区分语义收益、adapter 翻译成本和只有原生驱动才能实现的潜力。

## 2. 起点和问题陈述

现代 Vulkan、D3D12 和 Metal 已经把大量驱动工作交给应用，但应用和引擎仍重复维护：

- Buffer/Texture/Attachment 等资源对象；
- Bind Group、Descriptor Set、Root Signature 或 Argument Table；
- 每资源 usage/layout 状态机；
- 巨型 PSO permutation；
- CPU 生成 draw/dispatch 的中间命令；
- 资源生命周期、residency、地址和同步的多套身份。

No Graphics API 的启发是：对现代 64 位 GPU，普通应用数据可以更多地围绕 GPU 地址、root struct、bindless view 和 GPU 生成 Task 组织。

本项目进一步承认：

- 地址不等于 backing、lifetime 或 residency；
- 纹理逻辑值不等于线性字节；
- GPU 不能伪造 OS 控制平面；
- 动态 pointer graph 需要 sound 访问上界；
- 在途硬件 facet 需要版本固定；
- 故障通常不是事务回滚；
- adapter 可能隐藏翻译成本。

## 3. 根本目标

### G1：统一数据组织

应用结构、材质、几何、神经网络权重、Task payload 和普通计算数据都可以使用同一种类型化 GPU 地址图。数据组织不依赖每 draw 更新 binding slot。

### G2：统一访问语义

Buffer-like load、texture sample、storage store、attachment write、transfer 和 tensor/ray 访问都由 `Region + Layout + Access + ExecutionContract` 表达。硬件 ABI 可以不同，公共语义不分裂。

### G3：统一工作模型

CPU 和 GPU 能发布同一 Task ABI。GPU 可以在授权 envelope 内生成、排序、分桶和发布后续工作。

### G4：可证明同步

同步 API 主要表达 happens-before、visibility、representation version 和 ownership。它不要求应用维护公开 old/new usage state。

### G5：可测量的高性能

线性数据、静态 sample、root pointer、Task publication 和 Timeline 必须有 fast path 目标；所有 adapter 翻译、全局可见性、额外 pass 和 host-assisted 操作必须被记录。

### G6：可用而不是只漂亮

新手可以使用简单路径；专家可以查看地址、证书、facet、epoch、effect 和 lowering report。错误必须用这些概念解释，不要求用户读厂商 descriptor slot。

## 4. 非目标

- 当前不写通用开放 KMD 或私有 firmware。
- 当前不承诺跨厂商同一 ISA、同一 cache flush 或同一 residency 粒度。
- 当前不为浏览器/恶意不可信 shader 设计完整 sandbox 产品；只保留 profile 边界。
- 当前不追求支持十年前 GPU。
- 当前不把任意 GPU 动态 wait 变成可静态证明无死锁的语言。
- 当前不保证 adapter 结果一定比手写 Vulkan/D3D12/Metal 更快。

## 5. 成功标准

### 5.1 语义成功

- CPU reference 能执行至少 compute、linear load/store、pointer graph、Task publication、Timeline、epoch 和 poison。
- 同一源程序可生成 host schema 和 GPU schema，布局可查询且可重放。
- core model checker 能发现 stale pointer、未发布 Task、非法 ConsumeInput、缺失 happens-before、越权 Node 和 certificate 漏项。

### 5.2 后端成功

- M1 Metal 运行线性 pointer、sample facet、TaskTier0、ICB/indirect 和 shared event 实验。
- NVIDIA Vulkan 运行 BDA、descriptor indexing、timeline、indirect/execute-indirect 等可表达子集。
- 两种后端共享同一 core conformance suite。
- 任何不支持的语义都能返回结构化 lowering class，而不是错误地返回成功。

### 5.3 性能成功

至少有一组 workload 证明：

- CPU 不随 draw 数线性更新 binding 对象；
- root pointer 和 linear Region 没有额外 per-access descriptor indirection；
- GPU-generated same-Node batch 的 CPU 成本明显低于逐 draw baseline；
- certificate/lease 的成本和收益可以通过数据解释；
- adapter translation pass 的成本没有被隐藏。

### 5.4 负面结果也算成功

如果实验显示：

- adapter 只能通过 HostAssisted/TranslatedGraphPass 实现关键语义；
- certificate discovery 成本高于 residency 节省；
- RepresentationEpoch 多版本造成不可接受内存峰值；
- Task ABI 在现有后端必须反复重建传统 binding/state；

那么项目必须记录为架构边界，而不是用更多抽象掩盖失败。清楚证明“哪些能力只有原生驱动能实现”本身就是研究成果。

## 6. 工作假设

| 假设 | 最低要求 | 验证方式 |
|---|---|---|
| GPU VA 可被 shader 使用 | 64-bit address 或等价 backend | Metal/Vulkan codegen + ISA/assembly 检查 |
| 普通数据可 GPU 生成 | device-visible storage + atomic/publish | Task ring 实验 |
| timeline 可跨提交 | Metal event / Vulkan timeline / fallback | event conformance |
| texture 需要专用表示 | sample facet 或软件 fallback | view/facet tests |
| 无 fault 设备需要 sound lease | certificate 或 coarse universe | residency matrix |
| 动态 command 能力分层 | Tier0/1/2/3 | lowering report |

## 7. 关键术语

- **Region**：一个带类型、shape、layout、权限和地址域的逻辑数据视图。
- **Allocation**：物理 backing 和 storage policy。
- **Arena**：虚拟地址/所有权命名域，不等于完整驻留集合。
- **GraphEpoch**：pointer-bearing topology 的不可变发布版本。
- **RepresentationEpoch**：逻辑 Region 当前 backing map、metadata 和 facet 的版本。
- **AccessCertificate**：访问地址、表示和 effect 的 sound 上界。
- **WorkingSetLease**：一次执行对 backing/residency 的承诺。
- **Task**：发布后不可变的工作记录。
- **Node**：已注册、可验证的 Program 执行入口。
- **ExecutionEnvelope**：一次提交允许的 Node、access、lease、timeline、locality 和 quota 合同。
- **AccessWitness**：实际访问/版本/effect 的诊断证据，不替代静态证明。
- **Adapter**：把 VG 语义翻译到现有 GPU 授权入口的 DeviceHAL 实现；当前项目只实现 CPU reference、Metal 和 Vulkan，D3D12/CUDA 等不属于第一阶段。

## 8. 设计价值判断

本项目的“无特例”不是把一切强行塞入一个最大结构，而是：

1. 同一个逻辑数据/生命周期/effect/版本代数可以覆盖不同执行域。
2. 硬件私有表示只在按使用方式生成的 facet 中出现。
3. OS authority 不伪装成普通 shader 字节。
4. 性能代价在 capability 和 lowering report 中可查询。

如果某个提案只是把纹理、AI 或某厂商扩展单独加一个对象，它不是本项目的根本优化。

## 9. 目标追踪矩阵

| 根本目标 | 主要规范 | 最低实现证据 | 主要实验 | Phase gate |
|---|---|---|---|---|
| G1 统一数据组织 | 02 Region/typed ref；04 ABI；05 schema | 同一 root schema 在 reference/Metal/Vulkan | E002、E007 | B |
| G2 统一访问语义 | 02 Region/facet/effect；05 IR | linear/sample/attachment 共用 epoch/effect | E008、E012、E018 | C |
| G3 统一工作模型 | 02 Task/Envelope；04 Task ABI | CPU/GPU publication + Tier1 | E003、E009、E010、E017 | B/D |
| G4 可证明同步 | 02 memory model；03 submit | effect DAG + timeline + negative tests | E012、E015 | A/B |
| G5 可测量高性能 | 03 LoweringReport；08-10 | 无隐藏 host wait/descriptor/pass | E004、E007、E009、E013 | B-D |
| G6 可用与可诊断 | 04 diagnostics；08 capture | stable IDs、witness、replay | E006、E014、E015 | A/D |

如果某项实现无法映射到至少一个目标和证据，应解释它为何是必要基础设施，否则不进入当前路线图。
