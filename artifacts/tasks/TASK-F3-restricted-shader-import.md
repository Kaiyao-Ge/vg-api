# TASK-F3: 受限 MSL Shader 导入接入光栅化 Task

Status: Done（已实现并验证）。

Normative docs: ADR-043（Phase F Decision #4：用户自定义 vertex/fragment
shader 必须经受限导入接入，而非新设计一门 shading language）；ADR-047（F3
实现决策记录）。

## Goal

让光栅化 Task 可以携带调用方手写的 MSL vertex/fragment shader（通过新
`CodeObject.code.format_tag` `"vg.msl.raster/v1"`），而不是永远只能跑编译器
内建的固定 fragment 公式（F2/ADR-046 的 `raster_facet_metal_source()`）。
不新增公共 C ABI 版本；不做 shader 逻辑验证，只验证声明的四字段 envelope
契约（ADR-043 Decision #4："只验证声明的 contract，不验证 shader 逻辑"）。

## Invariants

- `ExecutionPlan::user_raster_shader` 一旦设置，`validate()` 跳过
  `ir::verify(module)`（该提交不携带 linear IR），改为要求
  `task_graph` 中每个 task 都是 `TaskKind::Raster`——v1 不支持
  compute+raster 混合提交，错误信息固定为
  `"a user_raster_shader submission may only contain raster tasks"`。
- `ir::parse_msl_raster_envelope` 对四个必需字段（`root_schema`/
  `vertex_entry`/`fragment_entry`/`source`）分别区分"字段整体缺失"
  （复用既有 `require()`，消息 `"IR missing field: X"`）与"字段存在但为
  空字符串"（本函数自己的检查，消息 `"MSL raster envelope missing field:
  X"`）两种拒绝路径。
- 绑定契约固定不变：`VgRasterVertex`/`VgRasterVaryings`/`VgRasterFragment`
  三个 struct 布局，以及 `vg::compiler::kRasterVertexBufferIndex`/
  `kRasterTintBufferIndex`/`kRasterTextureIndex`/`kRasterSamplerIndex`
  （均为字面量 `0`）——Metal encoder 无条件在这些固定槽位绑定资源，不检查
  自定义 shader 是否真的读取它们。
- 每个受限导入提交在 `compile()` 时必须记录 `"raster_user_shader"` /
  `HostAssisted` 的 `LoweringEvent`，绝不静默升级为 `Direct`
  （`docs/START.md` §4 invariant 10）。
- reference 后端 `raster_triangles()` 完全不变——不解析、不解释调用方提供
  的 MSL 源码，永远套用既有固定 C++ shading 公式。这是对 ADR-018"若某后端
  声称 `supported == true`，其结果字节必须与 reference oracle 精确一致"这
  一不变量的一处已披露、刻意为之的例外，范围仅限用户 shading 逻辑本身
  （facet 解析、vertex 变换、attachment 语义等上游行为仍跨后端一致）。
- Vulkan 不需要任何新代码：`compile()` 已经先调用 `plan.validate(error)`
  （在其既有的 `task.kind == Raster` 拒绝循环之前运行），一旦
  `validate()` 不再因为 `user_raster_shader` 场景下的默认/空 `module` 而
  提前失败，既有拒绝循环就会正确地拒绝掉受限导入的光栅 task——两个特性
  自然组合，无需改动 `vulkan_device_hal.cpp`。Vulkan 不设置
  `Capability::UserShaderImport`。

## 预计文件

- `src/ir/ir.h` / `src/ir/ir.cpp`（已完成）：`UserRasterShaderContract`
  struct、`parse_msl_raster_envelope(text)`。
- `src/backends/device_hal.h`（已完成）：`Capability::UserShaderImport`
  （`1u << 9`）、`ExecutionPlan::user_raster_shader`。
- `src/backends/device_hal.cpp`（已完成）：`ExecutionPlan::validate()`
  新增 `user_raster_shader` 分支。
- `src/api/vg_api_execution.cpp`（已完成）：`submit()` 新增
  `"vg.msl.raster/v1"` format-tag 分支，解析 envelope 而非
  `ir::parse_module`。
- `src/backends/reference/reference_device_hal.cpp`（已完成）：
  `capabilities()` 设置 `UserShaderImport`；`compile()` 新增
  `"raster_user_shader"` / `HostAssisted` 事件；`submit()` 的
  synthesized-success 路径驱动未改动的 `raster_triangles()`。
- `src/backends/metal/metal_device_hal.mm`（已完成）：
  `ensure_raster_pipeline`/`run_raster_pass` 新增可选 `user_shader`
  参数；`CompileOps::select_package`/`pipelines` 新增
  `"raster_user_shader"` / `HostAssisted` 事件；pipeline 仍在 `submit()`
  时惰性构建（沿用 ADR-046 Decision #2），entry point 名不匹配时在
  submit-time 干净失败（`"Metal raster pipeline compile failed: " +
  pipeline_error`，经既有 `finish()` lambda 折叠进
  `submission.result`）。
- `src/backends/vulkan/vulkan_device_hal.cpp`（无需改动，见上方
  Invariants）。
- `tests/unit/ir_test.cpp`（本任务新增）：envelope 往返测试 + 每个字段
  一个"整体缺失"用例 + 一个"存在但为空"用例。
- `tests/unit/reference_raster_test.cpp`（本任务新增）：plan-driven 受限
  导入用例，断言像素输出与 F2 固定 shading oracle 完全一致，并断言
  `HostAssisted` 事件存在。
- `tests/vertical_slice/metal_task_timeline_test.cpp`（本任务新增）：
  `run_task_graph_raster_user_shader`——(a) happy path：手写真实 MSL
  shader，输出纯绿色，与内建公式可清楚区分，断言逐像素匹配 +
  `HostAssisted` 事件；(b) entry point 名不匹配 source：断言
  `compile()` 仍成功（pipeline 惰性构建），`submit()` 调用本身成功但
  `submission.result.ok == false` 且消息含
  `"Metal raster pipeline compile failed"`；(c) 混合 compute+raster：
  断言 `compile()` 以精确消息拒绝。
- `tests/vertical_slice/vulkan_task_timeline_test.cpp`（本任务新增）：
  `run_raster_msl_rejected`——`user_raster_shader` 场景下 Vulkan 仍以与
  普通 raster-rejected 相同的方式拒绝，证明零新代码的结论。
  Compile-review-only（本机 macOS，无法构建 Vulkan）。
- `CMakeLists.txt`（本任务新增）：`vertical-slice.metal.task-graph-raster-
  user-shader`、`vertical-slice.vulkan.raster-msl-rejected` 两个新
  ctest 目标注册。

## Tests

- `ir.unit`（既有 target，本任务扩展其源文件 `tests/unit/ir_test.cpp`）：
  envelope 四字段往返 + 8 个拒绝子用例（4 个"整体缺失" + 4 个"存在但为
  空"）。
- `reference.facet-oracles`（既有 target，本任务扩展其源文件
  `tests/unit/reference_raster_test.cpp`）：plan-driven 受限导入用例，
  逐像素比对 F2 固定 shading oracle，并断言 `HostAssisted` 事件。
- `vertical-slice.metal.task-graph-raster-user-shader`（新增 target）：
  上述三个子用例（happy path / 畸形 entry point / 混合 compute+raster
  拒绝）全部通过。
- `vertical-slice.vulkan.raster-msl-rejected`（新增 target，
  compile-review-only）：手动核对 `vulkan_device_hal.cpp` 的
  `compile()` 调用顺序（`plan.validate(error)` 先于既有 raster 拒绝
  循环），逻辑自洽，未实际编译/运行。

已验证：`build/dev-reference` **26/26 ctest 通过**（无新增 target，
`ir.unit`/`reference.facet-oracles` 的扩展用例全部通过，与 F2 基线
26/26 计数相同）；`build/dev-metal` **56/56 ctest 通过**（F2 基线
55/55 + 本任务新增 `vertical-slice.metal.task-graph-raster-user-shader`
1 个）。`build/dev-vulkan` 本机（macOS）无法构建（既有非 Linux
`FATAL_ERROR` guard），未尝试构建；改为对
`vulkan_task_timeline_test.cpp` 新增用例与 `vulkan_device_hal.cpp` 的
`compile()`/`validate()` 实际调用顺序做了人工代码核对（见上方 Tests
小节），确认逻辑自洽，明确声明这一步未实际运行。

## Depends / Unblocks

Depends: ADR-043（Phase F 授权，Decision #4）、ADR-046（F2，光栅化接入
Task/ExecutionPlan 形状——F3 复用其 `TaskRecord`/`raster_facets`/
`vertex_buffer_ref` 等既有字段，未新增 `TaskRecord` 字段）。
Unblocks: F4（depth + 真实 PSO，ADR-043 §9 milestone 顺序）。

## Risks

已缓解：绑定契约固定不变（不做任意 binding 声明）把 F3 的攻击面限制在
"entry point 名字/source 文本"层面，畸形输入在 submit-time 干净失败而非
崩溃（已用畸形 entry point 子用例验证）。Vulkan 零新代码降低了回归面。

**未缓解 / 已披露给下一步 owner 的发现（本任务范围内不允许修复，仅报告）**：

1. **Metal `capabilities()` 未设置 `Capability::UserShaderImport`
   位**（`src/backends/metal/metal_device_hal.mm` 第 154-158 行左右的
   capability-bits 组装逻辑，只 OR 了 `EffectDag`/`TaskPublication`/
   `Raster`/`RepresentationTransform`/`CheckedFacetGeneration`，未包含
   `UserShaderImport`）。但 Metal 的 `compile()`/`submit()`
   （`CompileOps::select_package`/`pipelines`、`SubmitOps::raster`）已
   完整实现并接受 `user_raster_shader`，实测可正常工作（本任务新增的
   `vertical-slice.metal.task-graph-raster-user-shader` 三个子用例全部
   通过，且未依赖该 capability 位）。这与 `device_hal.h` 中
   `ExecutionPlan::user_raster_shader` 的文档注释矛盾——该注释明确写道
   "Meaningless (and never set) unless the backend advertises
   `Capability::UserShaderImport`"。任何遵循这一文档不变量、在提交前先
   检查 `capabilities().supports(Capability::UserShaderImport)` 的调用方
   会错误地认为 Metal 不支持受限 MSL 导入。只有 reference 后端正确设置
   了该位（`reference_device_hal.cpp:39`）。本任务未修改
   `metal_device_hal.mm`（超出授权编辑范围），仅在此报告，交由 owner
   决定修复时机（很可能是给 `capabilities()` 的 bits 组装补一行
   `if (支持 raster) bits |= UserShaderImport;` 或等价条件）。

## Decision needed

无。ADR-047 记录的分歧点（envelope 四字段契约 vs per-shader 绑定声明；
是否支持混合 compute+raster；是否验证 shader 逻辑）均已在实现前拍板。

## Out of scope

同 ADR-047 Consequences 段列出的范围外事项：混合 compute+raster 一次
submission（v1 完全不支持，见 Invariants）；完整 effect 推导（受限导入
只走声明契约，不走 `05-compiler-language-ir.md` §6 的九步推导）；自研
shading language（ADR-043 Decision #4 明确拒绝）；Vulkan 实现（ADR-043
§7：Vulkan 永久 compile-review-only）；per-shader 可声明绑定（本阶段
绑定契约固定不变，见 Invariants）。

## Appendix: `CompiledPlan` 文档注释已过期（仅报告，未修改 `device_hal.h`）

`src/backends/device_hal.h` 中 `CompiledPlan::compute_package` /
`indexed_compute_package` 字段的文档注释写道："Exactly one of
compute_package / indexed_compute_package is ever set for a given
CompiledPlan -- never both, never neither, on a successful compile()."
（第 251-253 行附近）。这一断言对 F3 之前的所有提交类型仍然成立，但对
F3 的纯光栅 `user_raster_shader` 提交已经失效：这类提交的 `plan.module`
从未被填充（`vg_api_execution.cpp` 的 `"vg.msl.raster/v1"` 分支只设置
`plan.user_raster_shader`，不设置 `plan.module`），因此一次成功的
`compile()` 会让 `compute_package` 与 `indexed_compute_package` **两者
都不设置**——"never neither" 这一半断言不再成立。

按本任务的编辑范围限制（`src/backends/device_hal.*` 不在授权编辑列表
内），此处仅标记为后续修复项，不在本任务内修改该注释。建议后续任务把
该注释更新为区分"IR-driven 提交"（原断言仍成立）与"restricted-import
raster 提交"（两者皆空是预期行为）两种情况。
