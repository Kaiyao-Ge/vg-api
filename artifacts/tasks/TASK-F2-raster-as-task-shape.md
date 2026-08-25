# TASK-F2: 光栅化接入 Task/ExecutionPlan 形状

Status: Done（已实现并验证）。

Normative docs: ADR-043（Phase F Decision #3：光栅化必须作为 Task/
ExecutionPlan 的一种形状而非并行 API）；ADR-046（F2 实现决策记录）。

## Goal

让光栅化（Metal `run_raster_triangles`、reference `raster_triangles`）
接入 `TaskGraph`/`compile()`/`submit()`，而不是只能被独立方法直接调用。

## Invariants

- `TaskRecord::kind` 默认 `TaskKind::Compute`，保证所有 F2 之前的调用方
  行为不变。
- `index_count > 0`（indexed draw 请求）在 `compile()` 时以 `Unsupported`
  拒绝，不静默降级、不悄悄忽略索引缓冲。
- 中性子集边界：`FilterMode`/`WrapMode`/tint 公开到 `TaskRecord`；
  `AttachmentLoadAction`/`AttachmentStoreAction`/`clear_rgba`/
  `sample_count`/`AttachmentSubresource` 留在后端私有类型
  （`AttachmentFacetDesc`），F2 用固定默认值（Clear/Store/{0,0,0,1}/1/
  {0,0}），不把 adapter 特性升级成核心最低能力。

## 预计文件

- `src/core/core.h`（已完成）：`TaskKind`/`Topology` 枚举，`TaskRecord`
  新增 `kind`/`topology`/`raster_facets`/`vertex_buffer_ref`/
  `index_buffer_ref`/`index_count`/`raster_filter`/`raster_wrap`/
  `raster_tint`。
- `src/backends/device_hal.h`（已完成）：`hal::RasterTaskResult`，
  `Submission::raster_results`。
- `src/backends/metal/metal_device_hal.mm`（已完成）：`Impl::
  run_raster_pass` 提取（原 `run_raster_triangles` 逻辑不改写，仅搬移）；
  `CompileOps::reject_unsupported` 新增 `index_count > 0` 拒绝；新增
  `SubmitOps::raster` 阶段。
- `src/backends/reference/reference_device_hal.cpp`（已完成）：
  `compile()` 同款 `index_count > 0` 拒绝；`submit()` 新增光栅任务执行
  （`execute()` 之后、task-graph 非空分支内）；`raster_triangles` 本体
  未改动。
- `tests/vertical_slice/metal_task_timeline_test.cpp`（已完成）：
  `same_task()` 比较器补齐新字段；新增 `run_task_graph_raster`。
- `tests/unit/reference_raster_test.cpp`（已完成）：新增 plan-driven
  光栅用例。
- `tests/unit/core_test.cpp`（已完成）：新增 `TaskRecord` 新字段默认值
  断言。
- `CMakeLists.txt`（已完成）：新增 `vertical-slice.metal.task-graph-raster`
  ctest 目标。

## Tests

- `vertical-slice.metal.task-graph-raster`（新增）：Metal 全路径
  `TaskGraph -> compile() -> submit()`，对照 reference oracle 校验像素；
  另有 `index_count > 0` 时 `compile()` 拒绝的子用例。
- `reference.facet-oracles` 内新用例（新增）：同一路径的 CPU 参照实现，
  断言 `submission.raster_results` 与直接调用 `raster_triangles` 结果
  逐像素一致；同样覆盖 `index_count > 0` 拒绝子用例。
- `core.unit` 新断言（新增）：`TaskRecord` 新字段默认值
  （`kind` 默认 `Compute`、`topology` 默认 `TriangleList`、
  `raster_filter`/`raster_wrap`/`raster_tint` 等）。

已验证：`build/dev-metal` 55/55 ctest 通过（含新增
`vertical-slice.metal.task-graph-raster` 与不变的
`vertical-slice.metal.basic-raster`）；`build/dev-reference` 26/26 ctest
通过（含更新后的 `reference.facet-oracles`(13) 与 `core.unit`(9)）。

### Post-review 修复（"Done"标记之后、提交之前）

一次针对 ADR-046/本文档的独立代码 review 发现两处问题，均已修复并重新验证：

- **Major-1（仅 Metal）**：`SubmitOps::raster` 原先在任一光栅任务失败时
  直接 `return false;`，被 `DeviceHal::submit()` 当作硬失败中止整个
  submit() 调用链——与 ADR-046 自己写明的"raster 失败不得阻止混合图里
  其余部分提交"矛盾。修复为纯软失败契约（`SubmitOps::raster` 只返回
  `bool`+`*out_message`，不碰 `submission->result`），并把
  `DeviceHal::submit()` 改为通过一个 `finish()` lambda，在链条其余阶段
  （`host_assisted`/`certificate`/`effect_dag`/`indexed`/`linear`）跑完
  之后才把 raster 结果折叠进 `submission->result`（且仅当此时
  `result.ok` 仍为 true 时才覆盖）——这个延迟是必要的，因为那些后续阶段
  各自在成功时无条件写 `result.ok = true`，若照搬 reference 后端的写法
  在 `raster()` 内部直接写 `submission->result`，会被 Metal 分阶段链条
  里后跑的阶段悄悄覆盖回 true。
- **Major-2（两个后端）**：顶点数用 `bytes.size() / sizeof(RasterVertex)`
  纯整数除法推导，没有余数校验，畸形顶点缓冲会被静默截断而非拒绝（非
  内存安全问题——下游读取仍受截断后的计数限制——但违反 `docs/START.md`
  §4 invariant 10 "不允许静默伪装"）。两个后端均已加
  `bytes.size() % sizeof(RasterVertex) != 0` 校验，命中则软失败。

新增回归测试（Metal）：`run_task_graph_raster` 内新增第三个子用例——构造
一个同时含一个普通 `Compute` 任务和一个 `vertex_buffer_ref` 从未 acquire
过（因此 `SubmitOps::raster` 必然失败）的 `Raster` 任务的混合图，断言
`submit()` 仍返回 `true`、`submission.result.ok == false` 且消息非空，
并且——关键点——`submission.result.trace`/`timeline_value`/
`published_tasks`（均只由跑完的 `SubmitOps::linear()` 写入）证明 compute
部分确实在 raster 失败的情况下跑完了。修复与新测试均已验证：
`build/dev-metal` **55/55 ctest 通过**（含此新子用例）；
`build/dev-reference` **26/26 ctest 通过**（不受影响——reference 的
raster 代码块本就跑在 `execute()` 之后，不存在 Major-1 同类问题）。

## Depends / Unblocks

Depends: ADR-043（Phase F 授权）、ADR-045（F1，`ExecutionResult` 经
C ABI 可达）。
Unblocks: F3（受限用户自定义 shader 导入）、F4（depth + 真实 PSO）、
F5（索引缓冲/真实 indexed draw）、F6（per-frame SceneRoot）、F9
（frames-in-flight，需要 compute<->raster 跨依赖）。

## Risks

已缓解：「搬移而非重写」（moved not rewritten）保证已通过硬件验证的
`run_raster_triangles` 路径行为不变（`vertical-slice.metal.basic-raster`
无回归）；Metal 与 reference 两个后端全量 ctest 通过。

## Decision needed

无。三个分歧点（`TaskRecord` 直接扩展 vs 并行 `raster_passes` 列表；
pipeline 惰性构建时机；中性子集边界）均已在 Plan Mode 中拍板，记录于
ADR-046。

## Out of scope

同 ADR-046 Consequences 段列出的范围外事项：`VgTaskRecord`（公共 C ABI）
不改；F5 之前不做真实 indexed draw；不做 depth（F4）、不做用户自定义
shader（F3）、不做 Vulkan 实现（ADR-043 §7：Vulkan 永久 compile-review-
only）；`Topology` 本阶段仅 `TriangleList` 一个值。
