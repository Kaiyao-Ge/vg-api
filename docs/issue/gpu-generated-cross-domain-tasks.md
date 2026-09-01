# 待研究问题：GPU 生成跨执行域 Task

状态：Open（未来 Tier 3 研究项；不是 Mixed-domain 实现要求）
记录日期：2026-09-01

## 背景

ADR-054 允许一张由 host 构建并由 Core seal 的 TaskGraph 在同一
`ExecutionPlan` 中组合 Compute、Raster 和未来执行域。它不要求 GPU 动态创建这些
Task。当前 GPU Task ring 是内部 compute-only Tier-0 publication wire：一个生成的
14×`uint32_t` record，保留 Compute dispatch 的 `x/y/z` 窗口，并明确拒绝 Raster。

这一窄定义应继续服务已有 Compute publication/indirect 实验，但不能自然表示 GPU 在
运行期间创建 Raster、Ray、Tensor/Neural 或 Video Task。为每个执行域复制一套
`*-only ring` 会制造多套 publication state machine、布局、shader codec、capture
格式和授权路径，违背统一 Task/Envelope 语义。

本问题记录一个未来方向：只有在真实 workload 需要 GPU 在授权 Envelope 内动态创建
跨执行域工作时，才研究一个版本化、带可验证 domain discriminator 的统一内部
publication schema。它对应 Task Tier 3，而不是当前 host-sealed Mixed-domain
TaskGraph 的前置条件。

## 与 ADR-054 的边界

- ADR-054 的 mixed-domain execution schedule 不依赖本问题完成。
- `Submission::published_tasks` 的完整图语义来自 sealed schedule + Envelope，不依赖
  某个 backend ring 能编码所有domain。
- 当前 compute-only ring可以保留为窄物理机制；不得把它升级为完整TaskGraph事实。
- 本问题不得被用来延迟Compute/Raster mixed-domain的Core schedule、Reference或
  Metal lowering。
- 在独立ADR、schema和conformance被接受前，GPU跨域Task生成保持Unsupported。

## 候选方向（非既定方案）

候选内部wire采用稳定公共头加domain-specific payload，而不是把所有执行域字段拼成
最大固定record：

```text
DomainTaskRecordHeader
├── schema/version
├── complete NodeRef {index, generation}
├── execution-domain discriminator
├── root identity
├── execution-shape class
├── contract index
├── payload size/offset
└── publication/continuation metadata

domain-specific payload
├── Compute dispatch shape
├── Raster draw/pass inputs
├── Ray dispatch/acceleration inputs
├── Tensor/Neural launch inputs
└── Video operation inputs
```

具体布局必须由schema generator产生或验证host、MSL、GLSL/SPIR-V、dump、capture和
relocation事实。现有compute ring的`x/y/z`与`VkDispatchIndirectCommand`兼容窗口可作为
Compute payload优化保留，但不能强迫其它domain采用Compute布局。

统一的是publication、authority、lifetime和effect语义，不是所有硬件命令的位级表示。
Backend仍把一个已验证record lower为compute dispatch、render/indirect draw、ray command、
tensor operation或明确Unsupported。

## 必须保持的硬边界

1. 不修改或尾扩展冻结的`VgTaskRecordV2`；内部wire若公开，必须使用新的版本化入口和ADR。
2. 不新增Raster-only、Ray-only、Tensor-only或Video-only publication协议。
3. 不新增平行的Allocation/Facet/Representation生命周期或domain-specific submit原语。
4. GPU不能伪造Node、generation、Timeline、外部ownership、FacetRef或未授权capability。
5. 每个生成Task必须受ExecutionEnvelope的Node集合、quota、AccessCertificate、GraphEpoch、
   RepresentationEpoch、working-set和delegation预算约束。
6. publication仍遵守`Empty -> Writing -> Published -> Consumed -> Empty`或经新ADR证明的
   等价release/acquire合同；consumer不得观察torn record。
7. Task进入执行schedule前必须得到sound effects、有限touched set和可验证contract；无法在
   GPU上证明时只能显式HostAssisted、defer到新commit或Unsupported。
8. Task ring、hardware work graph、ICB/indirect buffer或厂商command generator只是backend
   lowering，不能成为Node authority或第二套TaskGraph。
9. 所有host validation、translation pass、global visibility、queue split、descriptor生成和
   command expansion成本必须进入LoweringReport。

## 需要后续回答的问题

### 1. Schema与版本

- discriminator属于稳定头还是Node contract派生事实；二者不一致时在哪一层拒绝？
- 固定头和payload如何对齐、寻址、做bounds check和版本迁移？
- 如何保留Compute indirect窗口而不让它成为其它domain的最低公分母？
- capture/replay如何记录payload schema、relocation和未知domain的稳定拒绝？

### 2. Authority与动态验证

- GPU如何验证完整NodeRef generation及Node允许的ExecutionDomain/Contract？
- Envelope授权命名Node、Node class或预编译package集合时，GPU可见表如何避免成为较弱的
  index-only authority？
- 生成Task要求新的facet/representation、超出certificate或越过quota时，是拒绝slot、记录
  fault、产生continuation还是请求host commit？

### 3. Effect、access与schedule接入

- GPU生成Task的effects来自静态Node contract、payload解析、动态discovery还是保守effect class？
- 新Task如何加入ADR-054的component/wave schedule而不修改已经seal的执行事实？
- 是只允许向预授权continuation graph填充payload，还是允许真正扩展图拓扑？
- 动态Task依赖如何建立release/acquire、memory visibility、representation transition和
  cross-domain WaveTransition？

### 4. Backend映射

- Metal indirect argument/ICB、Vulkan indirect/work-generation能力分别能表达哪些domain和
  binding模型？不能表达的部分如何稳定归类为HostAssisted、SerializedFallback或Unsupported？
- 不同queue/encoder生成的Task如何进入统一completion/fault/timeline合同？
- GPU生成跨域工作是否真正减少host submit，还是仅增加translation pass和全局barrier？

### 5. Fault、overflow与可重放性

- 多producer同时发布失败时如何选择primary fault并保留其它诊断？
- ring overflow、Envelope quota、continuation budget和后继取消如何组合？
- `PartiallyProduced`范围如何覆盖已经发布但尚未执行、正在执行和已完成的动态Task？
- 哪些动态Task可安全retry；capture如何确定性重建生成顺序和payload？

## 触发条件

只有同时满足以下条件，才应把本issue提升为ADR/实现工作包：

1. 至少一个真实workload需要GPU创建非Compute Task，且host预建同一TaskGraph无法以可接受
   成本表达；
2. workload需要跨domain动态依赖，而不是单纯的domain-local indirect draw/dispatch；
3. 至少一个真实backend存在可验证映射，另一个backend可提供Reference或明确Unsupported；
4. 有可比较baseline证明收益来自减少host/translation成本，而不是隐藏同步或减少验证；
5. Node/Envelope/effect/access/capture/fault合同能够在实现前写成backend-independent
   conformance。

## 建议验证阶段

1. **Reference模型：** 版本化discriminator/payload、publication state machine、authority、quota、
   overflow、fault和capture round-trip。
2. **Schema闭合：** host/shader/pack/unpack/dump/relocation生成事实一致，fuzz未知domain、短record、
   stale NodeRef和非法payload。
3. **Compute兼容：** 新模型能够表达现有compute-only record，但不改变已有ring或Tier能力的行为。
4. **单一新增domain实验：** 只选择一个有真实硬件映射和可测收益的domain；不得同时展开
   Raster、Ray、Tensor和Video。
5. **跨域实验：** 验证动态dependency、WaveTransition、failure/poison、Envelope授权和
   LoweringReport准确性，再决定是否形成产品合同。

## 非目标

- 本issue不承诺通用hardware work graph、firmware scheduler或跨vendor原生命令格式。
- 不因统一wire而统一各domain的pipeline、facet或hardware representation。
- 不绕过Metal/Vulkan、OS ownership、driver validation或Device capability。
- 不为完成Mixed-domain整改而提前实现Tier 3。

## 关联资料

- [ADR-004：Task publication protocol](../decisions/ADR-004-task-publication-protocol.md)
- [ADR-021：Metal Task Tier0](../decisions/ADR-021-metal-task-tier0.md)
- [ADR-022：Vulkan Task Tier0/Tier1](../decisions/ADR-022-vulkan-task-tier0-tier1-timeline.md)
- [ADR-054：Mixed-domain execution schedule](../decisions/ADR-054-mixed-domain-execution-schedule.md)
- [Task与Envelope语义](../vg-project/02-principles-and-semantics.md)
- [Stage 0--7与LoweringReport](../vg-project/03-system-architecture.md)

## 关闭条件

只有当该问题被一个Accepted ADR明确裁决为“实现”“继续实验”或“拒绝”，并附带可重复的
Reference/backend证据、schema conformance、negative matrix和成本报告时，才能关闭。仅存在一个
能打包多种Task的struct或某个厂商API示例，不构成关闭证据。
