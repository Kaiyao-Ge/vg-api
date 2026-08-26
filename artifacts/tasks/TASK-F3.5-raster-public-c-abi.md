# TASK-F3.5: Raster 接入公共 C ABI

Status: Done（已实现并验证）。Agent 1 的 header 改动、Agent 2 的
`src/api/*.cpp` 插件层、Agent 3 的端到端测试均已完成，并已独立核实
（不只是转述 Agent 3 的自述）：`tests/api/vg_c_abi_conformance_test.cpp`
新增 +665/-0 行，未注册新 ctest target（仍是既有单一
`api.c-abi-conformance` 二进制）；`build/dev-reference` force-rebuild 后
`ctest` 26/26 全绿，`build/dev-metal` force-rebuild 后 `ctest` 56/56
全绿；两个 backend 上直接运行该测试二进制均退出码 0（配合其
`check()`/`g_ok` 模式，证明新增的全部断言——golden path、sub-case
(b)/(c)、v1.3 版本错位回归——均通过）。详见 ADR-048 `## Evidence`。

Normative docs: ADR-043（Phase F milestone 顺序，未命名 F3.5——F3.5 是 F3/F4
之间此前未命名的缺口，不是重开已定案的 F2/F3 scope）；ADR-046（F2，
raster 接入 `TaskRecord`/`ExecutionPlan` 形状，其 Consequences 明确写
"a public raster ABI entry point is deferred to a later F milestone"）；
ADR-047（F3，受限 MSL 导入）；ADR-048（F3.5 实现决策记录）。

## Goal

让公共 C ABI（`include/vg/vg.h`）第一次能够独立完成一次 raster 提交：
调用方可以构造一个 `VG_TASK_KIND_RASTER` 的 `VgTaskRecord`，通过新增的
`acquireFacet` 拿到它需要的 `VgFacetRef`（`raster_facets`/
`vertex_buffer_ref`/`index_buffer_ref`），提交并观察结果——全程只经过
`include/vg/vg.h`，不直接触碰 `vg_core`/`vg_backend_reference`/
`vg_backend_metal`。这是用户直接要求的、针对 F3 的一次真正端到端公共
C-ABI 测试的前置条件；此前不存在任何方式能通过公共 ABI 构造 raster
`VgTaskRecord` 或获取 `VgFacetRef`。

## Invariants

- `VgTaskRecord` 通过在结构体尾部**直接追加字段**增长（`kind`/
  `topology`/`raster_facets`/`vertex_buffer_ref`/`index_buffer_ref`/
  `index_count`/`raster_filter`/`raster_wrap`/`raster_tint`），不引入
  `VgStructHeader`，不引入 sentinel 标记——ADR-048 Decision #1 的结论。
- **Recompile hazard（已披露，非缺陷）**：`VgTaskRecord` 没有
  per-element `VgStructHeader`，`taskGraphAppend` 把它当作裸数组传递，
  没有像 `VgApi` 那样的按次 `size` 协商。追加 v1.3 字段改变了
  `sizeof(VgTaskRecord)`；用旧（更小）header 编译、链接到新 `libvg` 的
  调用方，其数组下标会按*库里编译进去的*更大 `sizeof` 计算，读出调用方
  自己数组的边界之外。调用方必须针对当前 header 重新编译。
- **Zero-init 默认值不匹配（已披露，非缺陷）**：零初始化的
  `VgTaskRecord` 能正确解出 `kind = VG_TASK_KIND_COMPUTE`、
  `topology = VG_TOPOLOGY_TRIANGLE_LIST`、`raster_wrap = VG_WRAP_CLAMP`
  （均为序数 0，与 `core::TaskRecord` 真实默认值一致），但**不能**正确
  解出 `raster_filter`（零解出 `VG_FILTER_NEAREST`，而
  `core::TaskRecord` 真实默认是 `FilterMode::Bilinear`）或
  `raster_tint`（零解出 `{0,0,0,0}`，真实默认是不透明白色
  `{1,1,1,1}`）。`task_graph_append` 对这些字段一律原样拷贝，不做
  default-substitution；调用方提交 `VG_TASK_KIND_RASTER` task 时必须
  显式设置这两个字段。
- Append-only ABI 增长纪律延续到第三次：`VgApi` v1.0→v1.1→v1.2→v1.3，
  `acquireFacet` 严格追加在 `getSubmissionExecutionResult`
  （v1.2 最后一个成员）之后，`vgGetApi` 的 `size` 边界级联新增一档
  （ADR-044/ADR-045 的既有模式）。
- `acquireFacet` 后端无关：通过 `hal::DeviceHal::facet_pool()`（三个
  backend 共享基类成员）实现，不像 F3 那样按 backend 分叉。
- `VgCanonicalViewDesc::allocation`/`allocation_generation` 是裸
  id/generation pair（不是 `VgAllocation` handle），与
  `core::CanonicalView` 的真实字段类型一致，复用既有
  `getAllocationRef` 机制。

## 预计文件

- `include/vg/vg.h`（已完成，Agent 1）：`VgTaskRecord` v1.3 字段块、
  新增 `VgCanonicalViewDesc`/`VgRasterFacetPair`、`VG_TASK_KIND_*`/
  `VG_TOPOLOGY_*`/`VG_FILTER_*`/`VG_WRAP_*`/`VG_PIXEL_FORMAT_*`/
  `VG_VIEW_DIMENSION_*`/`VG_SWIZZLE_*`/`VG_STRUCTURE_CANONICAL_VIEW_DESC`
  枚举块、`acquireFacet` 函数指针成员。已构建验证通过。
- `include/vg/vg_version.h`（已完成，Agent 1）：新增
  `VG_API_VERSION_1_3`/`VG_HEADER_VERSION_1_3 = 0x00010003u`。
- `src/api/vg_api.cpp`（已完成，Agent 2——已在 `git diff` 中核实）：
  `vgGetApi` 的 `size` 边界级联新增一档（`v1_2_size = offsetof(VgApi,
  acquireFacet)`，`at_least_v1_3` 分支把 `full_size` 扩到
  `sizeof(VgApi)`）；function table 把 `acquireFacet` 接到
  `vg_api::acquire_facet`。
- `src/api/vg_api_taskgraph.cpp`（已完成，Agent 2——已核实）：
  `task_graph_append` 的 `VgTaskRecord` → `core::TaskRecord` 逐字段拷贝
  已扩展覆盖全部 v1.3 新增字段（`kind`/`topology`/`raster_facets`/
  `vertex_buffer_ref`/`index_buffer_ref`/`index_count`/`raster_filter`/
  `raster_wrap`/`raster_tint`），源码注释里明确写了"no
  default-substitution"，与 Invariants 一致。
- `src/api/vg_api_facet.cpp`（已完成，Agent 2 新增文件——已核实）：
  `acquire_facet` 的实现，把 `VgCanonicalViewDesc` 逐字段翻译成
  `core::CanonicalView`，调用给定 `VgArena` 对应的
  `device->hal->facet_pool().acquire(...)`。
- `src/api/vg_api_internal.h`（已完成，Agent 2——已核实）：
  `acquire_facet` 的声明。
- `CMakeLists.txt`（已完成，Agent 2——已核实）：注册
  `src/api/vg_api_facet.cpp` 为构建源文件。
- `tests/api/vg_c_abi_conformance_test.cpp`（已完成，Agent 3——已独立
  核实）：新增 +665/-0 行，扩展既有 `api.c-abi-conformance` 二进制/
  ctest target（未注册新 target）。真正端到端、完全经过公共 C-ABI 的
  raster 提交测试用例，含 golden path、sub-case (b)/(c)、v1.3 版本错位
  回归用例。详见下方 Tests 段与 ADR-048 `## Evidence`。
- `docs/decisions/ADR-048-f3.5-raster-public-c-abi.md`（已完成，本任务
  新增）。
- `artifacts/tasks/TASK-F3.5-raster-public-c-abi.md`（已完成，本任务
  新增，本文件）。
- `docs/vg-project/04-public-c-abi.md`（已完成，本任务修改）：修复 §9
  预先存在的过期问题（补上 `root_generation` 与 `taskGraphAppend` 的
  `out_ids` 输出参数），并补充 v1.3 新增内容（新 §9.1）。

## Tests

`tests/api/vg_c_abi_conformance_test.cpp`（Agent 3，已完成，已独立核实
——单文件 +665/-0，扩展既有 `api.c-abi-conformance` 二进制，未注册新
ctest target）新增：

- **Golden path**：全程只经过 `include/vg/vg.h`——`vgGetApi` 请求
  `VG_API_VERSION_1_3` → `createRuntime`/`openAdapter`/`createDevice`/
  `createArena` → `loadCodeObject`（`format_tag = "vg.msl.raster/v1"`）→
  `acquireFacet`（`source = Sample`、`target = Attachment`、
  `vertex = Address`）→ `createNode`/`createTaskGraphBuilder` →
  `taskGraphAppend`（`VG_TASK_KIND_RASTER` 的 `VgTaskRecord`）→
  `sealTaskGraph` → `submit` → `getSubmissionExecutionResult`，断言
  canonical JSON 结果里 `"ok": 1`。
- **Sub-case (b)（仅 Metal）**：JSON envelope 声明的 `fragment_entry`
  在编译后的 MSL 源码里不存在。断言 `submit()` 本身仍返回
  `VG_SUCCESS`（host 侧接受），但 `getSubmissionExecutionResult` 报告
  `"ok": 0`，message 包含 `"Metal raster pipeline compile failed"`——
  证明 entry name 字符串确实驱动了 Metal 的函数查找，而不只是被
  JSON 解析走过场。
- **Sub-case (c)（backend 无关）**：混合
  `[VG_TASK_KIND_COMPUTE, VG_TASK_KIND_RASTER]` 的 task 数组，配
  `user_raster_shader` code object。断言 `taskGraphAppend` 成功，但
  `submit()` 返回 `VG_ERROR_INVALID_ARGUMENT`。
- **v1.3 版本错位回归用例**：`vgGetApi(VG_API_VERSION_1_2, ...)` 对
  v1.3 库调用，断言返回的 `size` 等于 `offsetof(VgApi, acquireFacet)`
  ——确认 append-only、`size`-gated 的版本协商纪律对老版本调用方仍然
  成立。

测试注释里同时明确披露了两个 API surface gap（非缺陷，是本任务范围外
的既有 v1.0–v1.3 缺口）：(Gap #1) 全版本都没有公开的
write-to-allocation 入口，测试里 acquire 到的 vertex/texture
allocation 必然是零填充；(Gap #2) 没有公开的 pixel-readback 入口，
无法通过纯公共 C-ABI 断言像素级正确性，因此改用 sub-case (b) 的
differential proof（正确 vs 故意写错的 `fragment_entry`）来证明
MSL 编译确实是真实发生的。详见 ADR-048 `## Evidence`/`## Consequences`。

构建与测试结果（独立核实，非转述）：`build/dev-reference` force-rebuild
后 `ctest` 100% 通过，**26/26**；`build/dev-metal` force-rebuild 后
`ctest` 100% 通过，**56/56**；两个 backend 上直接运行
`vg_c_abi_conformance_test` 二进制均退出码 0（配合 `check()`/`g_ok`
模式，`main` 返回 `g_ok ? 0 : 1`，退出码 0 即证明新增的全部断言都
通过）。

## Depends / Unblocks

Depends: ADR-046（F2，raster 接入 Task/ExecutionPlan 形状，本任务复用其
`TaskRecord` 字段语义）、ADR-047（F3，受限 MSL 导入，本任务是它在公共
ABI 上的可达性延伸，不是对它的重新设计）。

Unblocks: 用户直接要求的、针对 F3 的真正端到端公共 C-ABI raster 验证
测试（Agent 3 已完成，见 Tests 段）；F4（depth + 真实 PSO，ADR-043 §9
milestone 顺序，F4 现在可以假设 raster 已经是公共 ABI 可达的）。

## Risks

**Recompile hazard（已披露，见 Invariants；ADR-048 Decision #1
"Alternatives" 段已评估并拒绝 `VgStructHeader` retrofit 与 sentinel
两种缓解方案）**：`VgTaskRecord` 追加字段后，任何未针对新 header
重新编译就链接新 `libvg` 的调用方，会在 `taskGraphAppend` 的数组下标
计算中读出自己数组边界之外的内存。缓解手段是文档披露（`vg.h` doc
comment + 本 ADR），不是运行时检测——sentinel 无法生效，因为下标计算
本身在读到任何字段（含 sentinel）之前就已经算错。

Zero-init 默认值不匹配同样是已披露风险而非待修复缺陷（见 Invariants、
ADR-048 Decision #4）。

## Decision needed

无。ADR-048 记录的分歧点（`VgTaskRecord` 增长方式：直接追加 vs
`VgStructHeader` retrofit vs sentinel；`VgCanonicalViewDesc.allocation`
用裸 id/generation 还是 `VgAllocation` handle；`acquireFacet` 是否按
backend 分叉；zero-init 默认值不匹配是否需要 default-substitution 修复）
均已在本 ADR 中拍板。

## Out of scope

- 混合 compute+raster 一次 submission：已被 F3/ADR-047 Decision #6
  拒绝，本任务不重新讨论。
- 完整 effect 推导、per-shader 可声明绑定、自研 shading language、
  Vulkan raster 实际实现：均同 TASK-F3 Out of scope 段列出的范围，
  本任务不涉及，也不改变这些结论。
- 索引（indexed）raster 绘制：`index_count > 0` 仍在 `compile()` 时被
  拒绝（F5 才实现），本任务只是让"提交一个 `index_count == 0` 的
  raster task"这条路径公共 ABI 可达，不改变 indexed 绘制的既有拒绝
  逻辑。
- Depth：仍是 F4 的范围（ADR-043 Decision #5）。
- `04-public-c-abi.md` §9 之外的其他章节过期问题：本任务只修复
  §9 中与 `root_generation`/`out_ids`/v1.3 相关的部分，不对该文档做
  全面审计。
