# VG Project Agent Entry Point

状态：项目执行文档 v1.0

本目录是 VG（暂名，表示本项目提出的统一 GPU API）原型项目的唯一 Agent 入口。任何 Agent 在修改代码、设计接口、添加实验或解释架构之前，必须先阅读本文件，然后按照任务类型继续阅读对应文档。

## 当前事实状态

Correction（2026-08-23，执行入口，不是 `vg-project/*` 原文）：仓库已经有
CMake presets、公共 C ABI（`VG_API_VERSION_1_0` / `vgGetApi`）、portable
core、CPU reference、Metal adapter、Vulkan compile-review-only 源码、
`vg-exp` phase-a–phase-e、以及 A/B/D 的 gate 记录。Phase C 为
`not-closed`（layer1 complete）。Phase D 是研究记录（ADR-041），不是
Phase E 的自动入口。Phase E（Research Alpha）按 ADR-042 记录为
对外复现汇总，不是产品关门。干净 checkout 的构建与复现命令见
[reports/external-repro-runbook.md](reports/external-repro-runbook.md)。

上一版「尚未建立 CMake / runtime / 测试」的句子描述的是文档初稿时的仓库，
不再是当前 checkout 的事实。规范冲突优先级不变。

规范冲突优先级：本 `START.md` 的硬边界 > 01/02 的目标与语义不变量 > 03/04/05 的架构、ABI 与 IR 合同 > 06/07 的后端规则 > 08-10 的实验/验证规则 > 11-13 的执行建议 > 理论母本中的探索性设计。发现冲突时不得自行挑选方便的一项；记录问题并修改上位规范或 ADR。

## 0. 先读什么

1. 先读本文件，理解项目范围、当前硬件和禁止事项。
2. 任何架构/语义改动，读 [01-project-charter.md](vg-project/01-project-charter.md)、[02-principles-and-semantics.md](vg-project/02-principles-and-semantics.md)、[03-system-architecture.md](vg-project/03-system-architecture.md)。
3. 任何 C/C++ API 或 ABI 改动，追加阅读 [04-public-c-abi.md](vg-project/04-public-c-abi.md)。
4. 任何 Shader、编译器或 IR 改动，追加阅读 [05-compiler-language-ir.md](vg-project/05-compiler-language-ir.md)。
5. macOS/M1/Metal 任务，追加阅读 [06-backend-macos-metal.md](vg-project/06-backend-macos-metal.md)。
6. Linux/NVIDIA/服务器任务，追加阅读 [07-backend-linux-nvidia-vulkan.md](vg-project/07-backend-linux-nvidia-vulkan.md)。
7. 实验或 benchmark 任务，追加阅读 [08-experiment-system.md](vg-project/08-experiment-system.md)、[09-experiment-catalog.md](vg-project/09-experiment-catalog.md)、[10-validation-and-benchmarks.md](vg-project/10-validation-and-benchmarks.md)。
8. 项目执行、里程碑、风险或交接，追加阅读 [11-agent-workflow.md](vg-project/11-agent-workflow.md)、[12-roadmap-and-risks.md](vg-project/12-roadmap-and-risks.md)。

设计母本：

- `modern-native-gpu-api-architecture.md` 是完整理论草案，目前 v0.7。它不在当前 checkout 中；取得该外部母本后再将其作为补充阅读，不能阻塞本目录规范的执行。
- 新文档组将理论草案转换成可执行工程合同；若两者冲突，本目录的“项目硬约束”优先，理论草案中的未决问题必须标记为实验，而不能默认为已实现。

## 1. 项目一句话

构建一个以类型化 GPU 地址图、Region、Effect、不可变 Task、AccessCertificate、RepresentationEpoch 和 ExecutionEnvelope 为核心的统一 GPU 语义层；先在 CPU/reference、Metal 和 Vulkan adapter 上验证语义与性能，再评估是否存在值得实现的原生 UMD/KMD/firmware 合同。

## 2. 当前硬件边界

### 2.1 开发机：Apple Silicon MacBook Pro，M1

- 主要用途：CPU reference、编译器、Metal adapter、工具链、capture/replay、software rasterizer、低级同步与资源表示实验。
- 不能假定：可安装第三方通用 GPU kernel driver；可获得 Apple GPU 私有 ISA/firmware；Metal 可以被绕过。
- Metal 是本机 GPU 的授权入口。任何“原生 VG”结论在 M1 上都必须标记为 `MetalAdapter` 或 `SemanticReference`。

### 2.2 服务器：NVIDIA 50 系显卡

- 主要用途：Linux/NVIDIA/Vulkan adapter、GPU pointer graph、descriptor indexing、timeline、indirect/execute-indirect 类实验、性能 profiling。
- 可选：CUDA/HIP 仅作为计算实验 adapter，不作为项目语义或跨平台基础。
- 不能假定：NVIDIA 私有 firmware、命令包、KMD UAPI、硬件 work graph 或可恢复 fault 能被本项目直接接管。

### 2.3 明确不在当前项目范围

- 开放 KMD 的完整设计和实现。
- 自制 GPU、FPGA command processor、firmware scheduler。
- 绕过 Metal、WDDM、Linux DRM 或 NVIDIA 驱动的未授权硬件访问。
- 以 CUDA 语义代替跨平台语义。

## 3. 当前项目的三个轨道

| 轨道 | 目的 | 硬件依赖 | 允许的结论 |
|---|---|---|---|
| `PortableCore` | 验证语言、语义、生命周期、effect、模型检查器 | CPU/M1 | 证明语义自洽、错误可诊断、ABI 可设计 |
| `NativeAdapter` | 在真实 GPU 上验证 lowering 和性能 | Metal、Vulkan/NVIDIA | 证明某些 fast path 能映射到现有硬件；不能证明已有 API 概念在硬件中消失 |
| `NativeContractResearch` | 研究未来原生 DeviceHAL 合同 | 仅文档/模拟；未来可接开放硬件 | 评估 UMD/KMD/firmware 合同，不在当前机器上实现 |

Agent 不得把 `NativeAdapter` 的成功写成“VG 已经是比 Vulkan/Metal 更底层的 API”。

## 4. 根本不变量

以下不变量比任何具体后端 API 更优先：

1. 线性数据 fast path 可以降低为 GPU VA；静态路径不能被无意义 descriptor 间接拖慢。
2. 采样、attachment、storage、transfer 共享 Region/Representation 语义，但允许不同 hardware facet。
3. Task 发布后不可变；GPU 生成的 Task 不能伪造 Node、Timeline、外部 ownership 或未授权 capability。
4. AccessCertificate 必须是 sound over-approximation；不精确可以保守，不能漏项。
5. RepresentationEpoch 在途引用期间稳定；不得原地偷换活动 facet。
6. `ConsumeInput` 只在独占消费可证明时允许；它放弃回退和重放能力。
7. 同步的正确性来自 effect/happens-before；不能恢复逐资源 old/new usage 状态机作为核心语义。
8. 所有跨提交、跨进程、host/device、display ownership 的等待必须有 OS 可见 Timeline 或等价控制合同。
9. adapter 必须报告 lowering 成本，不能隐藏 descriptor、translation pass、host submission 或 global visibility。
10. 任何无法在当前硬件表达的语义必须返回 `Unsupported`、明确降级或进入 reference backend；不允许静默伪装。

## 5. Agent 的最小工作规则

- 先读相关文档，再看代码；不要先写 API 再补语义。
- 修改公共 ABI 前，更新 [04-public-c-abi.md](vg-project/04-public-c-abi.md) 和 ABI 版本策略。
- 添加后端 lowering 前，写出 `LoweringReport` 和对应 benchmark。
- 任何性能结论都必须有 baseline、数据、环境、方差和失败解释。
- 任何“统一”提案都必须回答：统一的是语义、生命周期、effect 还是位级表示？若只是位级强行统一，通常是错误方向。
- 不把 adapter 特性升级成核心最低能力。
- 不实现开放 KMD、firmware 或绕过 OS 保护的代码。
- 发现架构矛盾时先记录为决策/风险，再修改文档与测试，不要通过隐式特殊情况绕过。

## 6. 项目完成定义

第一阶段完成不是“有一个能画三角形的 demo”，而是同时满足：

- CPU reference 可以执行相同 Task ABI，并验证生命周期/effect/poison。
- Metal 和 Vulkan adapter 可以运行同一组 core conformance tests。
- root pointer、线性 Region、Task publication、Timeline 和 capture/replay 有实测路径。
- adapter 的隐藏成本可观测。
- 至少一个包含动态 pointer graph 的实验能比较 `CertifiedPinned`、`DiscoverThenLease` 和 `Universe`。
- 至少一个包含 RepresentationEpoch/ConsumeInput 的实验能报告峰值内存和故障/重放差异。
- 文档、代码、实验数据和结论可被另一 Agent 从本 `START.md` 重新定位。

## 7. 文档索引

| 文件 | 用途 |
|---|---|
| [01-project-charter.md](vg-project/01-project-charter.md) | 项目目标、范围、成功/失败定义 |
| [02-principles-and-semantics.md](vg-project/02-principles-and-semantics.md) | 语义原语、不变量、内存和版本模型 |
| [03-system-architecture.md](vg-project/03-system-architecture.md) | 四层依赖、组件边界、调用路径、对象关系 |
| [04-public-c-abi.md](vg-project/04-public-c-abi.md) | C ABI、handles、结构体、版本、错误和线程安全 |
| [05-compiler-language-ir.md](vg-project/05-compiler-language-ir.md) | Shader/host 语言、typed IR、effect、backend lowering |
| [06-backend-macos-metal.md](vg-project/06-backend-macos-metal.md) | M1/macOS/Metal 具体后端和限制 |
| [07-backend-linux-nvidia-vulkan.md](vg-project/07-backend-linux-nvidia-vulkan.md) | NVIDIA 50 系服务器与 Vulkan 后端 |
| [08-experiment-system.md](vg-project/08-experiment-system.md) | 实验基础设施、数据格式、运行协议 |
| [09-experiment-catalog.md](vg-project/09-experiment-catalog.md) | 逐项实验设计、变量、指标、判定 |
| [10-validation-and-benchmarks.md](vg-project/10-validation-and-benchmarks.md) | conformance、性能、公平 baseline、统计方法 |
| [11-agent-workflow.md](vg-project/11-agent-workflow.md) | Agent 如何接单、修改、测试、交接 |
| [12-roadmap-and-risks.md](vg-project/12-roadmap-and-risks.md) | 里程碑、决策记录、风险和停止条件 |
| [13-repository-layout.md](vg-project/13-repository-layout.md) | 建议代码仓库和产物布局 |
