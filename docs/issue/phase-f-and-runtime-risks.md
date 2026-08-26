# 待研究问题：Phase F 与任务图运行时风险

状态：Open（问题记录；非已确认缺陷、非既定解决方案）  
记录日期：2026-08-25

本文记录项目探索中识别出的三个需要后续设计、实验或性能分析来处理的风险。
它们均应以当前规范、ADR 与实测证据为准；在取得证据前，不应把任一风险的
潜在缓解路径当作项目承诺。

## 1. F3 受限用户 MSL 的语义验证边界与混合提交限制

### 现状

ADR-047 的 F3 路径允许调用者通过 `"vg.msl.raster/v1"` 导入受限的手写 MSL
vertex/fragment shader。该路径的安全与可用性建立在受限 envelope、固定字段契约、
后端编译和 `HostAssisted` 记录之上，而不是由 Portable Reference 重执行用户 shader
来证明每个像素结果正确。

因此，Reference 后端仍可验证 task 生命周期、effect/happens-before、资源与任务图
的公共语义，但不再是用户提供的 MSL 的像素正确性 oracle。当前 `ExecutionPlan`
还会拒绝同一次 `user_raster_shader` 提交中同时出现 compute 与 raster task 的情形。

相关依据：

- [ADR-047](../decisions/ADR-047-f3-restricted-msl-shader-import.md)
- [ADR-048：当前未覆盖项](../decisions/ADR-048-f3.5-raster-public-c-abi.md)

### 为什么这是风险

1. 传统的“Reference 输出与 GPU 输出一致”验证策略不能覆盖用户 shader 的颜色、插值、
   采样和 fragment 逻辑；即使任务图语义正确，也不能据此证明画面像素正确。
2. 后端编译成功、提交成功或 `HostAssisted` 事件存在，均不等于 shader 行为符合调用方
   意图；错误可能仅在特定驱动、GPU、输入或浮点行为下暴露。
3. 混合 compute+raster 被拒绝使 API 表达能力与部分真实渲染工作流脱节。调用方必须拆分
   提交或改走其他路径，可能引入额外同步、临时资源、性能成本或行为差异。
4. 如果以后扩大 shader 契约、绑定模型或后端覆盖范围，当前“固定字段 + 受限导入”的
   验证边界可能无法自然扩展。

### 需要后续回答的问题

- 应采用何种分层验证：shader 静态限制、shader-specific golden image、Metamorphic
  test、跨后端差分测试，还是可解释的受限 IR？每种方法可证明什么、不能证明什么？
- 对用户 MSL 应公开哪些保证等级（可编译、可提交、像素验证、跨后端一致性），并如何在
  API 结果与文档中避免把较弱保证描述成较强保证？
- compute+raster 混合提交是否是必须支持的语义目标；若是，应如何定义 task 顺序、资源
  effect、barrier/lowering、失败诊断和跨后端最低保证？
- `HostAssisted` 的成本、触发时机及其对可重放性和 capture/replay 的影响，是否应建立
  可量化的报告与门槛？

## 2. `VgTaskRecord` 尾部扩展造成的旧头文件/新库 ABI 风险

### 现状

v1.3 为公开 raster task，在 `VgTaskRecord` 尾部增加 `kind`、`topology`、facet
引用、index、filter、wrap 与 tint 等字段。该结构由
`taskGraphAppend(builder, const VgTaskRecord* tasks, uint32_t task_count, ...)`
作为连续裸数组传入；它不含逐元素的 `VgStructHeader` 或 size 字段。

若应用使用旧版头文件编译，而运行时链接新版 `libvg`，两者对
`sizeof(VgTaskRecord)` 的认识不同。库执行 `tasks[i]` 时使用的是新版本、更大的
步长，调用方实际数组则按旧版本、较小的步长排列。

```text
旧调用方内存： [ task 0 : old_size ][ task 1 : old_size ]
新库访问 task[1]：tasks + 1 * new_size
```

后一个地址可能位于旧 `task[1]` 的中间或数组末尾之后。即使只传一个元素，新库也会
读取 v1.3 新字段，而旧对象并没有为这些字段分配存储。因此风险是未定义行为，而不仅是
“新字段不可用”：可能导致任务字段错误、越界读取、崩溃或不可预测结果。

`VgApi` 函数表采用 append-only 与 `size` 协商，能保护旧客户端不访问新增的
`acquireFacet` 指针；但这套机制无法提供 `VgTaskRecord` 数组的逐元素步长，不能缓解
本问题。

相关依据：

- [当前 C ABI 定义及强制重编译说明](../../include/vg/vg.h)
- [ADR-048：Decision #1 与后果](../decisions/ADR-048-f3.5-raster-public-c-abi.md)

### 当前约束（F4 后）

对于 v1.3 及更早的 `taskGraphAppend`，升级到包含该结构布局的 `libvg` 时，任何调用方都
必须使用匹配的 `vg.h` 重新编译。旧头文件配新库不是受支持的组合；只请求旧版本 `VgApi`
也不能使旧版 `VgTaskRecord` 数组安全。

F4/ADR-049 不再延续该风险：深度字段放入新的固定布局 `VgTaskRecordV2`，且仅由 v1.4
函数表末尾的 `taskGraphAppendV2` 消费。原 `VgTaskRecord` 与 `taskGraphAppend` 固定为
v1.3 形状，F4 的库不会以更大的 stride 解读它。此举不能修复已经发布的 v1.2/v1.3
旧头/新库组合，但阻止了深度扩展制造第三次同类 ABI 破坏。

这是 ADR-048 明确披露的例外，而不是已实现的兼容层。ADR 当时因热路径的逐元素开销
拒绝给 `VgTaskRecord` 追加入 `VgStructHeader`；如果结构需要再次增长，或出现真实的
out-of-bounds incident，ADR 要求重新审视 plain-append 策略。

### 为什么这是风险

1. 动态链接、预编译 SDK、插件和二进制分发场景都可能让调用方在未察觉的情况下保留旧
   头文件编译产物。
2. 问题发生在库的数组指针算术阶段，早于读取任何可设计的 sentinel/magic 字段，因此
   普通运行时标记难以可靠拦截。
3. 这是公共 C ABI 的内存安全与发布兼容性约束；文档不足、包管理不足或错误的版本策略
   都可能把它转化为实际事故。

### 需要后续回答的问题

- `VgTaskRecordV2` 以后若需增长，应选择新的版本化 append 入口还是显式
  stride/element-size 的 batch API；不得再次尾部扩展 V2。
- 在不损害热路径目标的前提下，是否可提供安全的迁移入口、单元素 API、适配层或构建期
  ABI 检查？它们分别覆盖源代码、动态链接和第三方插件中的哪些风险？
- 发布物如何强制或清晰传达 header/library 配对要求，例如包版本、符号版本、ABI tag、
  configure-time 检查或 conformance test？
- 后续结构增长的决策门槛和兼容性测试矩阵应如何定义？

## 3. `submit` 与 `TaskGraphBuilder::seal` 的复杂度及潜在性能热点

### 现状

`submit` 是 API 编排汇合点：它验证 ABI header 与句柄、解析普通 IR 或受限 MSL raster
envelope、汇入 capability、graph epoch 与 execution envelope，调用
`DeviceHal::compile` 和 `DeviceHal::submit`，并保存 lowering/execution 结果。
其控制流已包含较多失败路径和跨层职责。

`TaskGraphBuilder::seal` 则将显式依赖与由 access effect 推导的冲突边合并，检查
happens-before 约束与环。现有冲突检测存在多层嵌套，任务数或 effect 数增大时，图构建
成本可能快速上升。

相关实现：

- [`submit` API 编排](../../src/api/vg_api_execution.cpp)
- [`TaskGraphBuilder::seal`](../../src/core/core.cpp)
- [HAL 执行边界](../../src/backends/device_hal.h)

### 为什么这是风险

1. `submit` 是 correctness、ABI、编译、后端调度、诊断和结果记录的交叉点。变更很容易
   影响多个后端或改变错误优先级；复杂控制流也提高测试遗漏与回归的概率。
2. 若每次提交都重复进行昂贵的 JSON/IR 解析、全图验证、编译或数据拷贝，小 task 或
   高频提交会让 CPU 端开销掩盖 GPU 工作本身。
3. `seal` 的冲突边推导若接近任务 × effect 的多重扫描，在大图、细粒度 Region 或复杂
   access 模式下可能成为吞吐和尾延迟瓶颈；其代价还会因 effect 拓扑而显著波动。
4. 为了优化而过早缓存、跳过验证或改变 effect 推导，可能破坏项目的 sound
   over-approximation 与 happens-before 不变量。因此性能优化必须与语义证据共同设计。

### 需要后续回答的问题

- 应建立哪些基准来拆分 `submit` 的成本：验证、解析、图构建、compile、后端提交、结果
  序列化，以及冷/热路径、不同后端和不同 task 大小？
- `seal` 的复杂度在当前实现与典型负载下实际是多少；其瓶颈是 effect pair 扫描、图环
  检测、内存分配还是诊断信息构建？
- 哪些输入可以在不削弱 soundness 的条件下预计算、增量维护或缓存；缓存的 key 又如何
  覆盖 code object、epoch、capability、facet 与 backend capability 的失效条件？
- 是否应把 `submit` 拆分为更易独立测试的验证、lowering/compile 和执行阶段；若拆分，
  错误语义、诊断与 execution result 的归属应如何保持稳定？

## 4. F4 raster facet effect 的精度与深度状态覆盖

### 现状

F4 已为带 depth attachment 的 Raster task 自动添加一个 `Write` effect。由于
`TaskGraphBuilder` 在 append 时不持有 `Arena` 或 `FacetPool`，该 effect 以完整
`FacetRef` 的 `(generation, index)` 编码为 synthetic identity，而不是 depth view 的真实
allocation/range。这样相同 depth capability 的任务必然推导出 WAW 边，避免在未来并行
lowering 中无序写同一深度面；它也不会漏掉相同 token 的冲突。

但不同 facet token 可以引用同一 allocation/view，builder 无法在当前阶段识别它们的别名；
synthetic identity 也可能与调用方提供的 allocation ID 偶然相同，从而只会多出一条保守依赖。
此外，实机 Metal 已覆盖 `Less + write` 的遮挡路径，Reference 覆盖八种 compare 的直接
oracle，但尚未形成完整的 Metal compare/write 矩阵、PSO/depth-state cache hit/miss 矩阵，
以及所有公共 ABI 负向输入的端到端矩阵。

### 潜在影响

1. 不同 token 指向同一深度 backing 时，缺少精确别名边可能在未来并行执行中造成 hazard。
2. synthetic ID collision 不影响 soundness，但会降低并行度并使图构建成本/诊断变得保守。
3. Metal 和 Reference 已有基础对照，但未覆盖的 state/cache 组合仍可能出现后端漂移。

### 需要后续回答的问题

- 是否应在 submit 时、拥有 envelope arena 后，将 raster source/vertex/color/depth facet 解析为
  实际 allocation range 并重建或补充 effect graph？该设计如何保持 sealed graph 的不变性？
- 是否要为 Raster task 引入显式、可验证的 resource-effect 描述，而非依赖 capability token
  的 synthetic identity？
- 应如何建立实机 Metal 的 compare × test × write × cache-key 矩阵，并纳入持续回归？
- 公共 ABI 应补齐哪些无效 enum、stale/wrong-kind facet、尺寸/层/mip 不匹配与旧 envelope
  的负向 conformance 用例？

## 5. F5 index facet 的非索引语义与负向覆盖

### 现状

F5 以 `VgTaskRecordV2` 既有的 `index_buffer_ref`/`index_count` 交付真实 u16/u32
indexed draw；index 元素类型由 Address facet 的 `R16Uint` 或 `R32Uint` canonical view
format 表示。Reference 与实机 Metal 已对非平凡四顶点索引 `{0,1,2,2,1,3}` 做完整颜色、
深度 oracle 对照，且 Metal 在提交期间保护 vertex/index facets 的 GPU 生命周期。

但当前执行路径仅在 `index_count != 0` 时解析 index facet。因此一个非 indexed task 若仍
携带非空 `index_buffer_ref`，该 capability 会被忽略，而不是得到明确诊断。另有短 buffer、
错误 format/错误 facet kind、越界 index 等实现级校验，但并非每一种都由纯公共 C ABI 和实机
Metal 的负向矩阵覆盖。

### 潜在影响

1. 调用方拼装 task 时多带了 index ref，可能误以为它参与了提交；这会掩盖 host 侧 record
   初始化错误。
2. 后端的校验虽已存在，负向回归不完整时，后续重构可能使 Reference/Metal 的诊断或拒绝时机
   漂移。
3. 与 F4 相同，不同 facet token 指向同一 allocation 的精确 alias/effect 关系仍未在 builder
   层表达；跨 task 的 producer-consumer 目前应显式声明 dependency。

### 需要后续回答的问题

- 是否将非 indexed task 的非空 `index_buffer_ref` 规定为 API 级 invalid argument，而非忽略？
- 是否将 index short-buffer、wrong-kind/format、out-of-range 和 stale facet 组合纳入 Metal 与
  纯 C ABI 的持续负向 conformance？
- raster resource effects 应何时从 capability-token 级跟踪升级为实际 allocation/range 级？

## 处理原则

在问题解决前：

- 不把 Reference 对 F3 用户 MSL 的覆盖范围描述为像素正确性证明。
- 不支持旧 `VgTaskRecord` 头文件/二进制与新 `libvg` 的混用；发布与示例应显式要求重编译。
- 不以移除 effect/happens-before 验证或隐式降级来换取 `submit`/`seal` 的性能。
- 任何候选方案应补充 ADR、跨后端 conformance、负向测试与可重复的性能证据。
