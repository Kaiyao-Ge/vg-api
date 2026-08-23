# 09 Experiment Catalog

本目录是第一阶段实验的规范。每项都必须有 hypothesis、对照、变量、指标、正确性和判定；实现时拆成 `experiments/E###-name/README.md` 与 machine-readable definition。

## 1. 通用约束

- 所有 GPU 实验先通过 CPU/reference 小规模 correctness；
- 所有性能变体使用相同输入、算法、输出和精度；
- 报告 VG lowering class；
- 每项至少 3 cold、50 warmup、10 batches，具体迭代由耗时调整；
- 首轮探索不设胜负结论，正式 run 预注册 threshold；
- Unsupported 与 HostAssisted 本身是结果。

## E001：生命周期与 epoch conformance

**问题**：Allocation/GraphEpoch/RepresentationEpoch 能否在无传统资源状态机下防止 stale reference？

**系统**：CPU reference，随后 Metal/Vulkan validation smoke。

**用例**：正常 allocation/use/retire；地址复用；旧 Task 延迟执行；facet epoch 变更；destroy while in-flight；capture replay。

**Oracle**：合法程序结果准确；每个非法用例在确定阶段返回 `STALE_HANDLE` 或 contract violation，不访问复用对象。

**通过**：100% deterministic 检出；无 false rejection；随机状态机 100k 序列无未分类错误。

## E002：类型化 pointer graph

**问题**：root pointer + typed graph 是否能表达复杂场景数据，并保持 direct fast path？

**输入**：数组、链表、树、DAG、带 cycle 图；locality linear/shuffled；1K-16M nodes。

**变体**：VG typed address；backend BDA/device pointer；descriptor/index table；flat packed baseline。

**指标**：CPU build/update、GPU traversal、bytes/read、cache proxy、address indirection、certificate size。

**正确性**：reachable node hash/count exact。

**判定**：VG direct path不得多出 per-node descriptor lookup；动态图性能下降需能由 memory locality 而非 API bookkeeping 解释。

## E003：Task publication memory model

**问题**：CPU/GPU producer 能否用同一 ABI 发布完整 Task，consumer 不观察 partial write？

**变体**：CPU->GPU、GPU->GPU、multi-producer；release/acquire 正确版本；故意缺 fence 的 negative fixture。

**压力**：随机 payload、ring wrap、quota boundary、millions of publishes。

**指标**：throughput、contention、retry、overflow、barrier cost。

**通过**：正确版本零 torn Task；negative fixture 被 validator/litmus 观察或标记，不要求错误硬件必现。

## E004：AccessCertificate 模式

**问题**：static、discovery、Universe 在动态 pointer graph 上的成本边界是什么？

**参数**：Arena size、reachable fraction 1%-100%、page locality、graph mutation、reuse count。

**变体**：CertifiedPinned；DiscoverThenLease；Universe；SoftwarePaged；FaultManaged 只在真实支持时。

**指标**：discovery GPU/host time、扫描/结果 bytes、lease/submit、working set、fault/eviction proxy、总时间。

**判定**：找出 discovery break-even 曲线；证书必须覆盖 witness；若 adapter 需要 host round-trip，结论明确限制 GPU autonomy。

## E005：ConsumeInput 与内存峰值

**问题**：显式独占消费能否降低频繁 representation transform 的峰值而不破坏 correctness？

**工作负载**：linear->sample optimal、mesh/tensor compaction、streaming double buffer。

**变体**：默认 multi-version；ConsumeInput；传统显式 fence+manual reuse baseline。

**指标**：peak/steady committed、temporary、transform time、stall、fragmentation、retire delay。

**故障注入**：transform 前/中/后 fault；capture replay request；外部引用存在。

**判定**：ConsumeInput 只在 proof 成功时启用；减少的水位与失去的 replay/rollback 明确量化。

## E006：AccessWitness 与诊断

**问题**：预期访问域和实际访存差异能否定位裸 pointer 错误？

**注入**：越界、错 allocation、错 generation、未声明写、错误 representation、data race。

**变体**：full instrumentation、sampling、guard zones、backend validation only。

**指标**：检测率、误报、运行/内存开销、定位到 source/Task 的精度。

**通过**：full reference 检出全部 fixture；native checked 对其声明覆盖范围不漏报；每条报告含 Node/Task/Region/source span。

## E007：root pointer 与传统绑定成本

**问题**：root struct 能否消除 per-draw/per-material CPU binding 更新？

**工作负载**：10^2-10^6 objects/material references，固定 shader work。

**变体**：VG root pointer；Metal/Vulkan 推荐 bindless/argument indexing；传统 descriptor/argument update；BDA native baseline。

**指标**：CPU data build、descriptor writes、encode、submit、GPU、memory、pipeline count。

**公平性**：相同批次、shader算法、可见对象、缓存预热；不拿逐 draw 最差写法当唯一 baseline。

**判定**：VG CPU overhead 随对象数增长主要来自普通数据写，而非 API object updates；GPU 不因额外 indirection 显著退化。

## E008：统一 Region 的 SampleFacet

**问题**：纹理作为 Region representation/facet 是否既统一语义又保留采样硬件效率？

**输入**：2D/array/mip、常见格式、linear/nearest、wrap modes。

**变体**：VG SampleFacet；native texture API；software linear sampler（仅语义对照）。

**指标**：facet create/cache、descriptor/argument writes、sample throughput、transform bytes、image quality。

**判定**：warm cached facet 接近 native texture路径；软件 sampler 不作为性能目标；不支持 format 返回明确类别。

## E009：GPU-generated same-Node work

**问题**：授权 envelope 内的 GPU cull/compact -> indirect execute 能否显著降低 CPU work？

**工作负载**：百万实例 culling，输出同 Node draw/dispatch。

**变体**：CPU cull+commands；GPU cull+readback+CPU；GPU cull+indirect；VG Task Tier1。

**指标**：CPU frame time/submits、GPU cull/execute、bubble、task bytes、overdraw。

**判定**：VG Tier1 至少匹配后端推荐 indirect；report 不出现隐藏 host round-trip。

## E010：异构 Node lowering

**问题**：GPU 选择多个预授权 Node 时，现有后端如何逼近统一 Task ABI？

**输入**：2-256 Node classes，偏斜/均匀分布，动态生成数量。

**变体**：native DGC/ICB（若有）；compute bucket+per-Node indirect；host readback；CPU sort baseline。

**指标**：bucket/sort pass、temporary、command/pipeline switches、CPU、GPU、latency。

**判定**：产生能力/成本曲线，不要求 VG 必胜；若必须 host-assisted，则标明原生 DeviceHAL 研究价值。

## E011：Residency 与 working set

**问题**：地址图与 backing/residency 分离是否在超大工作集下仍可控？

**输入**：工作集从 10% 到超过可用 budget；uniform/hotset/stream/random。

**变体**：whole Arena、manual ranges、discovery lease、sparse（Vulkan 可选）。

**指标**：budget/resident proxy、allocation/eviction/stall、GPU time、fault/device loss、quality degradation（若策略允许）。

**安全**：逐步扩大，设置内存上限和 watchdog；不导致系统不可用。

**判定**：给出每平台可用策略，不把 unified memory 当无限内存，不把 Vulkan sparse 当自动 fault。

## E012：Effect DAG 与 timeline

**问题**：effect/happens-before 是否能生成正确且不过度保守的 backend sync？

**图形**：linear chain、fork/join、independent branches、cross queue、representation transition、external present。

**变体**：VG inferred；backend hand-tuned；global barrier；intentional missing edge。

**指标**：barrier/encoder split/queue wait、overlap、GPU/CPU、validation errors。

**判定**：结果正确；independent branch 保留可用并行；与 hand-tuned 差距能由 backend限制或优化缺失解释。

## E013：Pipeline 与 specialization explosion

**问题**：Node/StateBlock 分类是否减少无意义 pipeline permutation？

**参数**：material/state combinations、format、sample count、dynamic state、function constants。

**变体**：naive full permutation；backend recommended dynamic state；VG classification/cache。

**指标**：pipeline count、compile wall/CPU、cache size/hit、frame hitch、GPU performance。

**判定**：VG 不增加必须固定的组合；可 shader-data/dynamic 的状态不会进入 pipeline key；fallback correctness 保持。

## E014：Capture/replay 与跨 backend

**问题**：canonical schema/IR/Task capture 能否定位同一程序并在兼容 backend 重放？

**用例**：compute exact、raster tolerance、dynamic graph、RepresentationEpoch、fault capture。

**变体**：same device、driver update（条件允许）、Metal<->Vulkan semantic replay、CPU reference。

**指标**：capture size/time、replay setup、output diff、unsupported reasons、stable ID quality。

**判定**：portable capture 不依赖 backend addresses；不兼容能力明确拒绝；same-environment deterministic用例 hash一致。

## E015：Fault 与 poison

**问题**：非事务 GPU 故障后，系统能否诚实标记不可信输出并保留诊断？

**注入**：bounds trap、invalid Node、timeout simulation、backend device fault（安全可控）、host cancellation。

**检查**：submission/task/region poison propagation、timeline fault、后续读取拒绝、device-lost transition、capture availability。

**通过**：不把部分输出报告成功；不假设 rollback；reference 与 adapters 使用相同 fault taxonomy。

## E016：RepresentationEpoch 高频流转

**问题**：多版本在连续 transform/streaming 中是否造成不可接受水位？

**参数**：frames in flight 1-8、transform frequency、resource size、GPU latency、memory budget。

**变体**：multi-version；backpressure 限制 epoch；ConsumeInput；drop/quality policy（应用显式允许）。

**指标**：epoch count、peak memory、producer stall、latency、throughput。

**判定**：形成 backpressure policy；禁止无界创建版本；内存不足时可预测失败而非系统抖动。

## E017：Envelope quota 与溢出

**问题**：高度动态 Task 展开如何在 OS 可控与 GPU autonomy 间取舍？

**工作负载**：adaptive subdivision、work stealing、递归宽度/深度变化。

**变体**：大静态 quota；分段 continuation；overflow buffer + 下一提交；host-assisted envelope extension 模拟。

**指标**：利用率、浪费预留、overflow、latency、host round-trip、fairness。

**判定**：第一阶段选出 portable continuation 机制；firmware micro-kernel extension 只作为模拟研究，不声称可部署。

## E018：数据图修改与 epoch publication

**问题**：对“修改数据与状态”的统一理解能否覆盖 topology 与 representation 更新？

**流程**：构建新 graph/representation -> validate -> release publish新 epoch -> in-flight旧读者完成 -> retire旧版。

**变体**：copy-on-write、chunk versioning、exclusive ConsumeInput、错误的 in-place mutation。

**指标**：copy bytes、peak memory、publication latency、reader stalls、错误检测。

**判定**：所有修改都用同一 version/publication法则表达；优化不同但无隐式特例。

## 2. 阶段门槛

| Gate | 必需实验 |
|---|---|
| Semantic Core | E001、E003、E006、E015、E018 reference 通过 |
| Compute Adapter | E002、E004、E007、E009、E012 在 Metal/Vulkan |
| Representation | E005、E008、E016 在 Metal/Vulkan |
| Research Alpha | E001-E018 均有结果或明确 Unsupported/Deferred |
