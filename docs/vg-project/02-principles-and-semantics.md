# 02 Principles and Semantics

本文件是 VG 的语义合同。它描述“程序是什么意思”，不描述 Metal/Vulkan 如何实现。

## 1. 语义分层

### 1.1 数据平面

数据平面包含应用可以生成和读取的普通数据：root structs、GPU pointers、relative references、Region/View payload、Task records、material/geometry/tensor/AI data，以及 LocalCounter 和 work queue payload。数据平面应该尽量是 GPU 可读写内存，CPU/GPU 可以对称产生；它不能携带未授权的 OS capability。

### 1.2 控制平面

控制平面包含必须由 runtime/driver/OS 注册或验证的实体：Runtime、Adapter、Device、AddressDomain、NodeRef、CodeObject、Timeline、WorkingSetLease、ExecutionEnvelope、external ownership、display endpoint、fault/reset、quota、priority 和 preemption。

控制平面不是“旧 API 对象包袱”，而是安全和调度 authority。它可以被压缩、缓存、批处理，但不能被普通 shader 任意伪造。

## 2. 类型化引用

概念上的引用至少包含：

```text
(AddressDomain, virtual address or relative offset,
 Type/Layout contract, permissions, provenance, generation)
```

在静态、私有、稳定 VA fast path 中，绝大部分元数据可在编译时擦除，只留下地址。跨进程或 VA 不稳定时使用 `SharedRef<T>`；facet 使用受授权的 pool index/generation；普通整数不能隐式转成 GPU pointer。

## 3. Region

```text
Region<T, Shape, Layout, Access, AddressDomain>
```

### 3.1 Layout

Layout 描述逻辑坐标如何映射到物理表示：`Linear`、`Tiled`、`SampleOptimal`、`StorageOptimal`、`Attachment`、`Tensor`、`Accel` 和 `Video` 等。后几类是 layout/representation trait，不是新资源生命周期。

### 3.2 Access

访问类别包括 `Read`、`Write`、`Atomic`、`SampleRead`、`AttachmentWrite`、`Publish`、`Transfer`、`Metadata`、`Instruction` 等。它们用于 effect 推导和 backend lowering，不是公开 old/new usage 状态机。

### 3.3 Facet

一个 CanonicalView 可以按使用生成 AddressFacet、SampleFacet、StorageFacet、AttachmentFacet 和 TransferFacet。facet 是硬件访问 token 的版本化后端表示。不能把所有 facet 拼成一个最大 ViewRecord，也不能在活动引用中原地修改。

## 4. 生命周期代数

### 4.1 Allocation/Address/Epoch

- Arena 管理地址/所有权命名域。
- Allocation 管理 backing/storage policy。
- Epoch 决定何时可回收地址、数据或 facet。
- GraphEpoch 冻结 pointer-bearing topology。
- RepresentationEpoch 冻结当前 backing/metadata/facet 解释。
- Task、Envelope、CodeObject 都遵守 build -> release -> immutable -> retire。

### 4.2 ConsumeInput

`ConsumeInput` 是一般独占消费 effect：

```text
old representation --consume transform--> new representation
```

使用前必须证明旧 envelope 已完成、没有外部引用、旧版本不会被重放、失败语义可接受。成功后旧 generation 失效。它可用于 texture retile、buffer compaction、tensor reorder、sparse tile remap。

## 5. Task 与 Envelope

### 5.1 Task

最小语义记录是：

```cpp
struct TaskHeader {
    NodeRef node;
    GpuAddress root;
    ExecutionShape shape;
    ExecutionContract contract;
};
```

实际 ABI 可把 contract 放入 Node template 或 envelope，避免每个 Task 重复复制大状态。Task 发布后不可变；GPU 只能写尚未发布 slot。

### 5.2 Node

NodeRef 是 capability index + generation，不是任意 ISA 地址。Node 表属于控制平面。Node 包含 CodeObject 入口、ExecutionDomain/Contract、静态链接接口、默认/允许 StateBlock、facet/argument lowering metadata、quota、fault 和 indirect 支持声明。

### 5.3 ExecutionEnvelope

Envelope 是 OS/driver 能审核的摊销单位，至少包含：

- 授权 Node 集或 Node effect class；
- AccessCertificate/GraphEpoch/RepresentationEpoch；
- WorkingSetLease；
- Timeline dependency；
- LocalityScope；
- Task quota、priority、preemption class；
- DelegatedEnvelope budget（若有）。

GPU 可以在 envelope 内自由生成细粒度 Task；越过 envelope 需要新 commit 或预授权 DelegatedEnvelope。

## 6. Effect 和内存模型

基本关系：`sequenced-before`、`publish-before`、`synchronizes-with`、`happens-before` 和 `data race`。两个重叠非原子访问若没有 happens-before，即为 data race。

Task graph 规则：同一 SerialLane 建立完成顺序；ParallelLane 只保证接收；显式 Event/Timeline 建立跨提交关系；effect/certificate 可自动产生边；无法证明不相交时保守扩大范围；LocalityScope 内部可使用 tile/on-chip 数据，离开 scope 必须 store/transform 后才能作为普通 Region 消费。

## 7. ResidencyMode

```cpp
enum ResidencyMode {
    CertifiedPinned,
    DiscoverThenLease,
    FaultManaged,
    Universe,
    SoftwarePaged,
};
```

### 7.1 CertifiedPinned

提交前已有 sound certificate，按 `ResidencyGranule` 展开 lease。无 recoverable fault 时这是默认安全路径。

### 7.2 DiscoverThenLease

先运行无副作用 discovery Node，遍历已驻留 seed topology，输出实际可达 granule witness，再压缩成 certificate 和 lease。GraphEpoch 和影响地址选择的输入必须冻结。

### 7.3 FaultManaged

设备可以恢复 page fault，lease 主要是预算/优先级提示。fault 仍需定义 Task、Timeline 和 poison 影响。

### 7.4 SoftwarePaged

应用管理 page table/indirection，使用 zero/fallback tile 或 explicit callback。它不是透明系统 fault；shader 必须知道访问结果可能是 fallback。

### 7.5 Universe

证书保守覆盖指定 Arena 的全部 allocation/backing。它适用于规模可控、无法证明精确可达集且设备不支持 recoverable fault 的路径；必须受 `universe_budget` 限制并报告实际字节。Universe 是 sound fallback，不是无限地址空间，也不能静默跨越未授权 Arena。

## 8. 表示转换和同步分离

`decompress`、`retile`、`resolve`、`format conversion` 和 `fast-clear eliminate` 是 representation transform，不是纯 barrier。它们产生新 RepresentationEpoch；barrier 只表达旧版本生产完成和新版本可见。

同步动作由以下版本/authority 统一描述：

| Effect | 版本 | 典型执行代理 |
|---|---|---|
| store/copy | MemoryContentEpoch | shader/copy engine |
| transform | RepresentationEpoch | copy/compute/fixed function |
| sparse map | AddressMapEpoch | queue/firmware/page-table authority |
| code specialization | CodeEpoch | compiler/driver compiler |
| Task publish | TaskQueueEpoch | release/acquire/command processor |
| present | OwnershipEpoch | OS/compositor |
| Timeline | TimelineEpoch | scheduler |

## 9. Fault 和 poison

Task fault 不是事务回滚。fault 之前的 store 可能已经可见，因此输出标记为 `Poisoned`/`PartiallyProduced`；未启动的依赖 Task 取消；已并行启动的 Task 可能扩大 poison 范围；自动 retry 只允许幂等、独立 destination epoch 或有 rebuild recipe 的 Task；强恢复使用双缓冲、copy-on-write 或 checkpoint。

## 10. AccessCertificate 和 AccessWitness

Certificate 是静态/提交期 sound over-approximation；Witness 是运行期实际观测：

```text
actual - certificate = 可能越权/漏声明
certificate - actual = 可能过度驻留/同步
effect trace vs graph = race/stall 线索
facet generation vs epoch = stale token 线索
```

Witness 不能替代 sound proof；一次运行没有访问某地址，不代表所有输入都不会访问。

## 11. 动态性分层

| 层级 | 机制 | 代价 |
|---|---|---|
| 可证明动态 | static certificate + frozen GraphEpoch | 约束最大，但最稳定 |
| 可发现动态 | discovery + lease + witness | 额外扫描和 seed residency |
| 可委托动态 | recoverable fault / DelegatedEnvelope | 强硬件/firmware/OS 依赖、复杂故障语义 |

项目所有 API 都要标记自己属于哪一层，不能把高层能力暗中降成低层能力。
