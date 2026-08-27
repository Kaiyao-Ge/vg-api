# Thermo-Nuclear 代码质量审查记录

本文含两次审查。第一至三节是 2026-08-26 对 `main` @ `b6bd6db` 的 `src/`、`tools/`、`include/` 快照。第四节起是 F6 SceneRoot 落地后的第二次审查：范围扩大到整个仓库（含 `tests/`、`CMakeLists.txt`、`schemas/`、公共样例与文档—代码一致性），并单独核对 F6 差量。两次审查的正文都不改写对方的观察；后一次若修正前一次的范围限制，会显式标明。

所用标准始终是 Cursor Team Kit 的 `thermo-nuclear-code-quality-review` skill：通过门槛不是「行为看起来正确」，而是没有明显的结构退化、没有可见却未做的大幅度简化、没有无正当理由的超大文件、没有靠在既有控制流上叠加特判来增长功能、没有把实现细节泄漏到错误的分层。

---

## 第一次审查（2026-08-26）：`src/` / `tools/` / `include/` @ `b6bd6db`

- 审查对象：分支 `main`，提交 `b6bd6db`（`Adding host I/O`），与 `origin/main` 一致。
- 审查范围：仅 `src/`、`tools/`、`include/`。测试、实验定义、ADR、CMake 不在该次行数统计与结构裁决之内，但在解释成因时引用 ADR。
- 裁决：按该标准，**当时落地代码不能作为继续堆叠功能的健康基线**。下文把「看到了什么」「如何解释」「准备怎么改」分开写。

---

## 一、事实

这一节只记录可复核的观察：审查是怎么做的、哪些文件被打开、文件里实际有什么、调用关系实际怎样。不在这一节做价值判断。

### 1.1 审查过程

本次不是针对某个 pull request 的 diff 审查。当时工作区已经在 `main` 上，相对 `main` 的 diff 为空，因此审查对象是这三棵目录在 `HEAD` 上的整树快照。

过程按官方 skill 的编排执行，分三步。

**第一步：范围与体量。** 在仓库根目录执行：

```text
git rev-parse --abbrev-ref HEAD
git status -sb
git log -1 --oneline main
git ls-tree -r --name-only main -- src tools include
```

并对 `git ls-tree` 列出的每个路径做 `wc -l`，按行数降序排列。得到 58 个文件、合计 20 963 行。同时用 `git log --oneline -20 main` 看最近的功能堆积方向。

**第二步：结构地图，两路并行。**

- 一路用 shell 抽出超过约 800 行的文件的结构大纲：顶层 `namespace` / `class` / `struct` / 方法定义的行号、`#if` 分区、Metal 与 Vulkan `DeviceHal` 公开方法名对照、`static_cast` / `reinterpret_cast` / `std::optional` / 特性宏的出现次数。
- 一路按分层阅读 `include/vg/vg.h`、`src/api/`、`src/core/`、`src/compiler/`、`src/backends/`、`src/ir/`、`src/capture/`、`tools/`，追踪 `DeviceHal`、`ExecutionPlan`、`vgCreate*` / `submit`、后端特判与跨层 `#include`。

**第三步：对照 rubric 做裁决。** 父代理把体量快照、结构大纲、以及从源文件中读出的关键片段（`ExecutionPlan` 字段表、Metal `submit` 的实际调用顺序、Vulkan `compile` 对 raster 任务的拒绝、API 层的解析与工厂、core 头文件的类型清单）交给 `thermo-nuclear-code-quality-review` 子代理。子代理按 skill 规定的优先级输出；父代理再回读下列文件核对行号，避免把结构推断写成事实：

| 核对文件 | 核对内容 |
|---|---|
| `src/backends/device_hal.h` | `Capability`、`ExecutionPlan`、`DeviceHal` 虚函数、共享 stage 自由函数声明 |
| `src/backends/device_hal.cpp` | `ExecutionPlan::validate`、`run_representation_stage` 等定义 |
| `src/backends/metal/metal_device_hal.h` | 基类三个 override 之外的公开方法 |
| `src/backends/metal/metal_device_hal.mm` | `pack_task_record`、`CompileOps`、`SubmitOps`、`DeviceHal::submit` |
| `src/backends/vulkan/vulkan_device_hal.h` | 额外公开方法与 `Vk*` 成员 |
| `src/backends/vulkan/vulkan_device_hal.cpp` | `VG_HAS_VULKAN` 双编译、`compile`/`submit`、注释块与真实调用 |
| `src/backends/reference/reference_device_hal.cpp` | `submit` 对共享 stage 的调用顺序 |
| `src/core/core.h` | 类型清单、`ExecutionEnvelope::apply_to` 的声明与放置说明 |
| `src/api/vg_api_internal.h` | 句柄包装与 `VgAllocation` 例外 |
| `src/api/vg_api_device.cpp` | 后端工厂 |
| `src/api/vg_api_execution.cpp` | `apply_to` 定义、`submit` 中的 IR / MSL 解析 |
| `src/api/vg_api_arena.cpp` | `reinterpret_cast` 把 `Allocation*` 当成 C 句柄 |
| `tools/vg-exp/vg_exp.py` | Phase 映射与 runner 形状 |
| `src/compiler/compute_package.cpp` | 文件职责 |

另外用 codebase-memory 知识图对 `src/` 做了一次架构查询（节点 1519、边 4228）。该图用于交叉验证跨包调用方向，不替代打开源文件。知识图给出的跨包 `CALLS` 边包括：`backends → core` 34、`backends → ir` 17、`backends → compiler` 16、`core → ir` 10、`backends → api` 4、`api → backends` 4、`core → api` 2、`core → backends` 1。也就是说，在「api 使用 backends、backends 使用 core」的预期方向之外，图上还能看到反向边。

未做的事情，以免把审查范围说大：没有跑测试、没有做运行时剖析、没有审查 `tests/` 与 `docs/` 正文、没有对每一个小于 100 行的文件做语句级走读。小于 100 行的文件只进入了行数表和分层归属。

### 1.2 范围的定量快照

`git ls-tree -r --name-only main -- src tools include` 列出 58 个路径。按 `wc -l` 降序，完整行数如下。

```
4364  src/backends/metal/metal_device_hal.mm
3984  src/backends/vulkan/vulkan_device_hal.cpp
1510  src/core/core.cpp
1044  tools/vg-exp/vg_exp.py
 992  src/core/core.h
 759  src/backends/reference/reference_executor.cpp
 639  src/compiler/compute_package.cpp
 637  src/backends/vulkan/vulkan_device_hal.h
 600  src/backends/metal/metal_tier2.mm
 558  include/vg/vg.h
 550  src/backends/device_hal.h
 484  src/capture/capture.cpp
 420  src/backends/reference/reference_device_hal.cpp
 408  src/backends/metal/metal_device_hal.h
 336  src/backends/reference/reference_executor.h
 321  src/api/vg_api_execution.cpp
 300  src/backends/device_hal.cpp
 260  src/compiler/compiler.h
 242  src/api/vg_api.cpp
 232  src/api/vg_api_taskgraph.cpp
 208  src/compiler/pipeline_classification.cpp
 193  src/api/vg_api_internal.h
 169  src/compiler/pipeline_classification.h
 128  src/api/vg_api_arena.cpp
 108  src/backends/envelope_stage.cpp
  98  tools/vg-capture-view/vg_capture_view.cpp
  98  src/api/vg_api_device.cpp
  89  src/api/vg_api_code.cpp
  86  src/capture/capture.h
  82  tools/vg-schema/vg_schema.py
  79  src/api/vg_api_facet.cpp
  77  src/backends/working_set_stage.cpp
  75  src/ir/ir.cpp
  70  src/backends/discovery_stage.cpp
  60  tools/vg-docs/vg_docs.py
  60  src/ir/sha256.cpp
  52  tools/vg-golden-gen/vg_golden_gen.cpp
  51  tools/vg-platform-probe.cpp
  49  src/ir/ir.h
  47  src/ir/json.cpp
  46  src/backends/metal/metal_tier2.h
  44  src/backends/reference/tier2_oracle.cpp
  43  src/backends/vulkan/vulkan_probe.cpp
  43  src/api/vg_api_handle_registry.h
  39  src/backends/reference/tier2_oracle.h
  30  src/backends/metal/metal_probe.mm
  25  src/ir/json.h
  23  tools/vg-reference.cpp
  23  src/backends/reference/reference_probe.cpp
  21  src/backends/probe.h
  20  src/core/task_schema.cpp
  19  include/vg/vg_version.h
  14  src/core/task_schema.h
  14  src/compiler/compiler.cpp
  13  tools/vg-replay.cpp
  11  tools/vg-compile.cpp
  10  src/backends/reference/reference_device_hal.h
   6  src/ir/sha256.h
-----
20963 合计
```

按扩展名：`.cpp` 32、`.h` 20、`.py` 3、`.mm` 3。

超过 1000 行的文件有四个：`metal_device_hal.mm`、`vulkan_device_hal.cpp`、`core.cpp`、`vg_exp.py`。`core.h` 为 992 行。前两个文件合计 8348 行，占审查范围的 39.8%；前五个合计 11894 行，占 56.7%。

`src/`、`tools/`、`include/` 内 `TODO` / `FIXME` / `HACK` / `XXX` 匹配数为 0。`static_cast` 约 380 处，主要集中在两个 GPU HAL 与 `reference_executor.cpp` 的整数收窄与枚举转换。`reinterpret_cast` 的实际出现点是 `src/api/vg_api_arena.cpp`（把 `core::Allocation*` 转成 `VgAllocation` 再转回来）和 `src/api/vg_api_execution.cpp:98`（证书范围里同样的指针还原）。`dynamic_cast` 为 0。`std::optional` 的集中点是 `device_hal.h` 的 `ExecutionPlan` / `CompiledPlan` / `Submission`，共 11 处字段。`std::variant` 仅 `src/ir/json.h` 的 JSON 值。`std::any` 为 0。

最近二十个提交的主题包括：host I/O、Indexed Raster 的 V2 合同、depth、用户提供的 Metal shader 经公共 C API 暴露、公共 C ABI 完成、以及一次把 Metal 的 compile/submit/validate/execute 拆成分段 helper 的提交（`093d0c8`）。Vulkan 一侧的 `compile` / `submit` 在本次快照里仍是各自一个函数，没有对等的 `CompileOps` / `SubmitOps` 分段结构。

### 1.3 目录分层（文件归属，不是评价）

物理目录给出的分层如下。

| 目录 | 角色（从 `#include` 与符号归属读出） |
|---|---|
| `include/vg/vg.h`、`include/vg/vg_version.h` | 公共 C ABI：句柄 typedef、描述符结构体、版本化 `VgApi` 函数指针表、`vgGetApi` |
| `src/api/` | C 入口实现：句柄表、`createDevice` / `submit` 等，直接包含 `core/core.h` 与 `backends/device_hal.h` |
| `src/core/` | 运行时语义对象：Arena、FacetPool、TaskGraph、证书、工作集、信封等 |
| `src/ir/` | 模块解析、校验、JSON、SHA-256 |
| `src/compiler/` | IR 到 MSL / GLSL 的包构建、管线分类 |
| `src/backends/` | `hal::DeviceHal` 契约、三个后端、以及 representation / discovery / working-set / envelope 的共享自由函数 |
| `src/capture/` | 捕获与对照产物 |
| `tools/` | CLI 与 `vg_exp.py` 实验编排 |

`hal::DeviceHal` 的虚接口实际只有三个方法，见 `device_hal.h:512-524`：`capabilities()`、`compile(...)`、`submit(...)`。另有两个非虚访问器：`facet_pool()`、`envelope_continuations()`。工厂方面，`make_reference_device_hal()` 声明在 `device_hal.h`；Metal / Vulkan 的 `make_device_hal` 声明在各自头文件。

三个后端都实现了 `compile` 与 `submit`。除此之外，Metal 与 Vulkan 的具体类上还有互不对齐的额外公开方法（见 1.5、1.6）。这些额外方法不在基类上，因此不能通过 `hal::DeviceHal*` 做后端中立调用。

### 1.4 `ExecutionPlan` 在头文件里实际有哪些字段

`src/backends/device_hal.h:137-241` 中 `struct ExecutionPlan` 是一个单一聚合类型。字段按源码顺序如下（省略注释原文，只记类型与名字）：

- `uint32_t abi_version`
- `CapabilitySnapshot capabilities`
- `ir::Module module`
- `core::Certificate certificate`
- `core::TaskGraph task_graph`
- `uint64_t graph_epoch`
- `uint64_t timeline_wait`、`uint64_t timeline_signal`
- `bool published`
- `std::optional<core::AccessCertificateMode> requested_certificate_mode`
- `bool request_tier1_indirect`
- `std::vector<ir::Module> effect_dag_passes`
- `std::vector<std::pair<uint32_t, uint32_t>> effect_dag_dependencies`
- `bool request_indexed_binding`
- `std::vector<RepresentationRequest> representation_requests`
- `core::ValidationProfile validation_profile`
- `std::optional<core::WorkingSetBudget> working_set_budget`
- `std::optional<core::WorkingSetLease> working_set_lease`
- `std::optional<core::EnvelopeOverflow> pending_overflow`
- `std::vector<core::PointerRef> discovery_seeds`
- `std::optional<uint32_t> envelope_task_quota`
- `bool request_tier2_select`
- `std::vector<uint32_t> authorized_node_classes`
- `std::optional<ir::UserRasterShaderContract> user_raster_shader`

方法：`validate`、`graph_epoch_matches`。

头文件注释反复使用同一模式：某字段的默认值（`false`、空 `vector`、`nullopt`）被定义为「保留该里程碑之前每一位调用者的行为」。`request_indexed_binding` 的注释（`device_hal.h:174-181`）还写明设计选择：方差放在 `ExecutionPlan` 字段上，而不是增加虚函数，以便 `DeviceHal::compile` / `submit` 保持两个方法。

`ExecutionPlan::validate`（`device_hal.cpp:102-135`）实际检查：ABI 版本、`user_raster_shader` 存在时跳过 `ir::verify(module)` 并要求任务全是 `TaskKind::Raster`、否则 `ir::verify(module)`、`LinearAddress` 能力位、timeline 单调性、已发布任务图、`representation_requests` 合法性、working-set lease 不超过 budget、`pending_overflow`、以及 `request_tier2_select` 时至少两个互异的 node class。它**不**检查 `request_indexed_binding` 是否与 `effect_dag_passes` 同时为真（该组合由 Metal `CompileOps::reject_unsupported` 拒绝）。它也**不**检查 `request_indexed_binding`、`request_tier1_indirect`、`request_tier2_select`、`discovery_seeds` 是否被当前 `capabilities` 所声明的能力位支持。

`Capability` 枚举（`device_hal.h:21-55`）是位掩码：`LinearAddress`、`TaskPublication`、`Timeline`、`EffectDag`、`CaptureReplay`、`IndirectTier1`、`Raster`、`RepresentationTransform`、`CheckedFacetGeneration`、`UserShaderImport`。注释把「广告某一位」定义为义务：广告了就必须真正执行对应语义，否则应保持该位清零并报告 `Unsupported`，而不是静默降级。

### 1.5 Metal HAL 文件实际构成

`src/backends/metal/metal_device_hal.mm` 共 4364 行，无 `#if defined(VG_HAS_METAL)`：该文件靠构建系统在非 Apple 主机上不编译，而不是靠预处理器做 stub。

可观察的大块：

| 大约行段 | 内容 |
|---|---|
| 26–150 | 匿名命名空间：能力探测、`pack_task_record` / `unpack_task_record`、`publish_envelope_order`、`make_hal_snapshot` |
| 151–449 | 格式 / 深度 / compare 转换、`DispatchStats`、`FacetUseGuard` |
| 451–约 2303 | 嵌套类型 `DeviceHal::Impl`：缓冲与 facet 缓存、sampler / pipeline 缓存、`ensure_*`、representation blit、task ring、effect DAG、raster pass、tier1 indirect |
| 2377–2632 | 嵌套类型 `DeviceHal::CompileOps` |
| 2635–3265 | 嵌套类型 `DeviceHal::SubmitOps`，内含 `enum class Flow { Fail, Finish, Continue }` |
| 3267–3298 | `DeviceHal::submit` 对 `SubmitOps` 的调用 |
| 3300–约 4304 | `probe_buffer`、`run_cull_compact`、`run_address_facet`、两套 `run_sample_facet`、两套 `run_storage_facet`、`run_attachment_facet`、`run_representation_transform`、`run_raster_triangles`、`run_pipeline_classification`、`last_tier1_indirect_dims`、`make_device_hal` |

`pack_task_record`（`metal_device_hal.mm:63-78`）把 `core::TaskRecord` 打成 14 个小端 `uint32` 字：`node_index`、`node_generation`、`root_allocation` 的低/高 32 位、`root_generation`、`x`/`y`/`z`、`flags`、`contract_index`、`payload_size`、填充 0、`payload_or_offset` 的低/高 32 位。函数体不读取 `task.kind`，也不读取 raster 专用字段。

`DeviceHal::compile`（约 2621 行起）按固定顺序调用：

1. `CompileOps::reject_unsupported`
2. `CompileOps::select_package`
3. `CompileOps::representation_requests`
4. `CompileOps::timeline`
5. `CompileOps::pipelines`

（结构大纲还列出 `effect_dag` 段；`compile` 的调用链以源文件当时读到的五段为准。）

`reject_unsupported`（2395–2410）拒绝 `SoftwarePaged` / `FaultManaged` 证书模式；并在「非 pointer-graph 模块且 `request_indexed_binding`」同时带有非空 `effect_dag_passes` 或非空 `task_graph` 时失败。

`DeviceHal::submit`（3267–3297）的实际控制流是：

1. `SubmitOps::begin`：校验、调用 `hal::run_discovery_stage`、调用 `hal::apply_working_set_budget`（2692–2693）。
2. `SubmitOps::stage5`：调用 `hal::run_representation_stage`，物理回调转到 `Impl::transform_into_private_facet`。
3. `SubmitOps::raster`：结果先放在局部 `raster_ok` / `raster_error`，不在此处覆盖 `submission->result.ok`。
4. 定义局部 lambda `finish`：若 raster 失败且后续路径把 `result.ok` 写成了 true，则改回 false 并填入 raster 错误。
5. 依次：
   - `SubmitOps::take(precheck_timeline(...), &result)` 若返回 true 则 `return finish(result)`
   - 对 `host_assisted`、`certificate`、`effect_dag`、`indexed` 做同样的 `take` + 提前返回
   - 否则 `return finish(linear(...))`

`SubmitOps::take`（2638–2642）的语义：`Flow::Continue` 表示本段不适用，调用方继续下一段；`Fail` 或 `Finish` 表示本段已经结束整次 submit，调用方应立即返回。因此这是一个「按字段探测、命中则退出」的线性调度器，而不是一张事先算好的模式表。

Metal 头文件 `metal_device_hal.h:273-356` 在三个虚函数 override 之外，公开了 `snapshot`、`probe_buffer`、`run_cull_compact`、`run_address_facet`、两套 `run_sample_facet`、两套 `run_storage_facet`、`run_attachment_facet`、`run_raster_triangles`、`run_representation_transform` 等。这些方法接受 `core::FacetPool&` 并解析 `FacetRef`，用于垂直切片与测试，不经过 `compile`/`submit` 的 `ExecutionPlan`。

同目录下已有 `metal_tier2.mm`（600 行）与 `metal_tier2.h`（46 行），说明至少有一次把 Tier 2 选路从主 `.mm` 抽到独立翻译单元的先例。

### 1.6 Vulkan HAL 文件实际构成

`src/backends/vulkan/vulkan_device_hal.cpp` 共 3984 行。它是**同一翻译单元的双编译**：`#if defined(VG_HAS_VULKAN)` 包裹真实 Vulkan 路径，`#if !defined(VG_HAS_VULKAN)` 为 `compile` / `submit` / `run_*_facet` / `create_impl` 提供 stub。头文件 `vulkan_device_hal.h` 同样用该宏包裹 `VkInstance` / `VkPhysicalDevice` / `VkDevice` 等成员（约 215–220 行一带），因此无 Vulkan 的构建仍编译这个类，只是成员集不同。

没有 Pimpl。缓冲、管线、facet image、task ring、descriptor pool 的状态直接作为 `DeviceHal` 的成员。

`pack_task_record`（`vulkan_device_hal.cpp:178` 一带）与 Metal 版本字段顺序、字数、位移方式相同。`compile()`（2137 行起）在校验 backend kind 之后，遍历 `plan.task_graph.tasks()`：若任一 `task.kind == TaskKind::Raster`，则填写 `LoweringClass::Unsupported` 并返回 false。紧挨着的注释写明原因：`pack_task_record` / `unpack_task_record` 不读 `task.kind`，若放行，raster 任务会落到 task-graph 发布路径上，被当成默认 `x=y=z=1` 的 compute dispatch。这是源码自己陈述的静默伪装风险，以及当前用 `compile` 入口处循环来堵住该风险的做法。

`compile()` 随后无条件调用 `compiler::build_linear_compute_package(plan.module)`（2184）。文件中 2008–2012 行的注释写明：`compile()` **从不**读取 `plan.request_indexed_binding`，**从不**调用 `compiler::build_indexed_compute_package()`。

`DeviceHal::submit`（2277 行起）在支持的构建里：检查 `compiled.report.supported` 与 `compute_package`、对齐 `graph_epoch`、做 timeline 预检、然后走 representation / buffer bind / dispatch / task ring。对该 `.cpp` 全文搜索 `run_discovery_stage` 与 `apply_working_set_budget` 的**调用**为零。这两个函数的「若接线会怎样」写在 1986–2016 行附近的多段注释里，标为 compile-review-only。

Vulkan 具体类上的额外公开方法包括 `run_sample_facet`（一套，带 `lod`）、`run_storage_facet`（一套）、`run_raster_facet`（不是 Metal 的 `run_raster_triangles`）、`run_pipeline_classification`（参数类型与 Metal 的 `PipelineClassificationRun` 不同），以及在 `VG_HAS_VULKAN` 下暴露 `instance()` / `physical_device()` / `device()` / `compute_queue_family()`。

### 1.7 Reference 后端实际构成

`reference_device_hal.cpp` 的 `submit`（134 行起）顺序是：

1. `hal::run_discovery_stage`
2. `hal::apply_working_set_budget`
3. `hal::run_representation_stage`（物理回调把 `distinct_backing` 设为 false，因为 host 字节数组就是最优表示）
4. 若存在 `user_raster_shader` 则跳过 `execute(module)`，否则跑 IR 解释器
5. 若任务图非空：`hal::apply_envelope_continuation`，再在 host 侧 `PublicationRing` 上发布
6. 光栅任务走 `raster_triangles()`

`compile` 在 `consume_input == true` 时拒绝（119–128）：恒等变换没有可提前释放的旧 backing。

`reference_executor.cpp`（759 行）在同一文件内包含：IR 指令解释（load/store/atomic/pointer）、`execute` / `execute_task_graph` / `cull_compact`、texel I/O 与采样滤波、`sample_facet` / `storage_facet` 重载、`attachment_facet`、以及软件光栅 `raster_triangles()`。

### 1.8 Core 头文件与实现文件实际装了什么

`src/core/core.h` 992 行。文件顶部 `#include "ir/ir.h"`，并 `namespace vg::hal { struct ExecutionPlan; }` 前向声明。其中声明或定义的主要类型包括：

- `Allocation`、`ConsumeProof`、`Arena`
- `FacetKind`、`CanonicalView`、`FacetPool`、`RasterFacetPair` 及深度相关枚举
- `RepresentationEpoch` / `RepresentationEpochBuilder`
- `TaskRecord`（compute 与 raster 字段在同一结构体）
- `GraphEpoch`、`PointerGraph`、`PublicationRing`、`TaskGraph`、`EffectGraph`
- `Timeline`、`Certificate`、`AccessWitness`、`AccessCertificate`
- `DiscoveryResult`、`WorkingSetBudget`、`WorkingSetLease`、`EnvelopeOverflow`、`EnvelopeContinuationTable`
- `ExecutionResult`
- F1 ABI 支撑：`AddressDomain`（一个 `uint32_t kind`）、`CodeObject`、`NodeTable`、`ExecutionEnvelope`

`ExecutionEnvelope::apply_to(hal::ExecutionPlan&)` 在 `core.h:982-987` 声明。注释写明定义放在 `src/api/vg_api_execution.cpp` 而不是 `core.cpp`，目的是避免 `vg_core` 依赖 `backends/` 头文件。定义本体在 `vg_api_execution.cpp:18-28`：把 `allowed_node_classes`、可选证书模式、可选 task quota、timeline wait/signal 写进 `plan`。

`src/core/core.cpp` 1510 行，实现上述类型的方法，没有按子域拆成多个 `.cpp`。

### 1.9 公共 C ABI 实现层实际做了什么

`src/api/vg_api_internal.h` 同时 `#include "backends/device_hal.h"` 与 `"core/core.h"`。`VgDevice_T` 持有 `unique_ptr<hal::DeviceHal>`。注释写明 `VgAllocation` 是例外：不是 registry 跟踪的包装结构，而是直接的 `core::Allocation*` 转换。

`vg_api_arena.cpp:83-85`：`arena_allocate` 在 `arena->arena.allocate(size)` 之后执行 `*out_allocation = reinterpret_cast<VgAllocation>(&allocation)`。`get_allocation_ref` / `write_allocation` / `read_allocation` 用反向 `reinterpret_cast` 还原。`write`/`read` 在失败时用 `is_live_allocation` 区分 `VG_ERROR_INVALID_ARGUMENT` 与 `VG_ERROR_STALE_HANDLE`。

`vg_api_device.cpp:61-80`：`create_device` 按 `adapter->record.backend_kind` 做 `switch`，在 `VG_HAS_METAL` / `VG_HAS_VULKAN` 下直接调用 `vg::metal::make_device_hal` / `vg::vulkan::make_device_hal`。

`vg_api_execution.cpp` 的 `submit()`（约 209–234）在填充 `hal::ExecutionPlan` 时：

- 若 `code_object->code.format_tag == "vg.msl.raster/v1"`，把字节当文本交给 `ir::parse_msl_raster_envelope`，结果写入 `plan.user_raster_shader`；
- 否则交给 `ir::parse_module`，结果写入 `plan.module`。

也就是说，格式标签分派与 IR / envelope 解析发生在 C ABI 的 submit 入口，而不是发生在 `src/ir/` 对 `CodeObject` 的加载点，也不是发生在 `src/compiler/`。

`include/vg/vg.h` 558 行，保持 C 类型与 append-only 的 `VgApi` 表。其中 `VgTaskRecord` / `VgTaskRecordV2` 附近有关于 v1.3 字段增长导致 sizeof 变化、以及与 `core::TaskRecord` 默认值不完全一致的注释。这是公共头文件自己记录的 ABI 风险，不是审查者外加的。

### 1.10 编译器与实验工具的实际形状

`src/compiler/compute_package.cpp` 639 行。它并行生成 MSL 与 GLSL（`build_pointer_graph_compute_package`、`build_linear_compute_package`、`build_indexed_compute_package`），并在同一文件内嵌 task ring 与 sample/storage/raster facet 的 shader 源字符串。`compiler.cpp` 本身只有 14 行。

`tools/vg-exp/vg_exp.py` 1044 行，没有 class。顶部是 Phase A–E 的实验 id 到 ctest 名的字典（约 21–184 行）。`PHASE_B_EXPERIMENTS` 的注释写明 Vulkan 在此从不执行，按 ADR-024 记为 compile-review-only。`create_phase_c_run`（484 行起）、`create_phase_d_run`（618 行起）、`create_phase_e_run`（764 行起）是三段结构相近的函数：建 run 目录、对定义循环跑 `ctest`、写 sample JSON / CSV / `report.md`。Phase B 的 report 文本固定写上 Vulkan samples 为 compile-review-only。

### 1.11 已经存在的共享提交阶段

下列自由函数已经声明在 `device_hal.h`、定义在 `device_hal.cpp` 或对应 `*_stage.cpp`：

| 函数 | 职责（按头文件注释） |
|---|---|
| `run_representation_stage` | Stage 5：epoch / facet / consume 的跨后端簿记；后端只提供 `physical` 回调 |
| `apply_working_set_budget` | 本 submit 的 residency 上限，超限拒绝而非静默截断 |
| `run_discovery_stage` | host 侧 DiscoverThenLease；空 seeds 为 no-op |
| `apply_envelope_continuation` | 按 quota 切分 `TaskGraph::deterministic_order` |

Metal `SubmitOps::begin` 调用 discovery 与 working-set；`stage5` 调用 representation stage。Reference `submit` 调用上述四个中的 discovery、working-set、representation，并在任务图路径上调用 envelope continuation。Vulkan `submit` 在本次快照中不调用 discovery / working-set 这两个 helper。

---

## 二、看法

这一节是对上一节事实的解释：我认为这意味着什么、问题的计算机科学名称是什么、以及为什么它会伤害后续修改。看法不等于已实现的重构。

### 2.1 通过门槛：正确性不是可维护性

三个后端都能为各自范围内的计划返回成功或 `Unsupported`，注释密度高，且大量引用 ADR。按「能不能跑」的标准，这棵树是认真写过的。Thermo-Nuclear 标准问的是另一件事：下一次功能（再一个 raster 形状、再一种 binding、再一种 host I/O 变体）会落在哪里。

我的判断是：它会继续落进 `ExecutionPlan` 的新字段、Metal `.mm` 的新 `SubmitOps` 段、Vulkan `.cpp` 的新注释块或新 `#if`，以及 `core.h` 的新类型。行数表已经显示这个过程发生过——前两个文件占四成代码——而最近提交仍在往同一方向加功能。这不是「将来可能变乱」，而是「乱的吸引子已经稳定」。

### 2.2 `ExecutionPlan` 是隐式的乘积类型，不是提交的规范形式

在类型理论上，一个记录（record / 乘积类型）的含义是「这些字段同时成立」。`ExecutionPlan` 把所有里程碑的开关、可选向量、可选合同放进同一个记录。于是类型允许的值空间是各字段值域的笛卡尔积。实际合法的提交只是这个积的一个很小的子集，例如：

- `request_indexed_binding` 与非空 `effect_dag_passes` 在 Metal 上被 `reject_unsupported` 拒绝，但类型仍允许同时为真；
- `user_raster_shader` 有值时 `module` 被约定为空，且 `validate` 跳过 `ir::verify`；
- `request_tier1_indirect` 在没有真实 GPU dispatch 的 reference 上「无意义，忽略」；
- Vulkan 的 `compile` 忽略 `request_indexed_binding`。

「默认值保留旧调用者行为」是一种向后兼容策略，但它把**时间上的版本差异**编码成了**空间上的可选字段**。每加一个里程碑，积类型的维数加一，每个 `compile`/`submit` 实现就要再读一维。这是经典的特征交互问题（feature interaction）：两个单独安全的特征，合在一起需要额外的拒绝规则，而拒绝规则散落在 Metal 的 `CompileOps`、共享的 `validate`、Vulkan 的循环补丁里，并不在类型里。

头文件把「保持两个虚函数」写成了把方差放入计划对象的理由。窄的虚接口本身合理（稳定的二进制与测试面）。不合理的是：用一个无标签的乘积去模拟本应用标签联合（tagged union / sum type）表达的「一次提交只有一种主意图」。结果是控制流承担了本该由数据构造器承担的分派——Metal `SubmitOps::take` 就是在运行时对乘积的各维做顺序探测。

`Capability` 位掩码描述的是适配器的义务；`ExecutionPlan` 字段描述的是调用方的愿望。`validate` 没有把愿望投影到义务上：一个广告了 Vulkan、未广告 indexed binding 的 snapshot，仍可带上 `request_indexed_binding == true` 并通过 `validate`，然后被 Vulkan `compile` 静默按 linear package 降低。能力位写得很认真，计划对象却允许写出「愿望超出义务」的值。这比诚实的 `Unsupported` 更难调试，因为成功路径看起来像是忽略了调用方请求。

### 2.3 三个后端对同一契约的行为不可替换

Liskov 替换原则要求：凡是使用基类型的代码，换上子类型后仍应保持基类型的可观察约定。这里的基类型是 `DeviceHal` 加上「`ExecutionPlan` 字段的含义」。实际上：

- Metal 实现了 indexed、Tier1、effect DAG、user raster、raster task graph，并调用共享 stage；
- Vulkan 拒绝 raster 任务、忽略 indexed 字段、不调用 discovery / working-set helper，raster 垂直切片走另一套 `run_raster_facet`；
- Reference 用 IR 解释器与软件光栅给出语义裁判，representation 是恒等变换。

它们不是三个可互换的适配器，而是三个共享字段名、不共享操作语义的实现。共享 `ExecutionPlan` 造成的后果是：每个实现必须对「自己读不懂的维」做忽略、拒绝或注释。Vulkan 选择了第三种——在热路径文件里用散文描述反事实实现。这是把规格文档焊进 `.cpp`：读者必须先消化未接线的映射，才能读到真正执行的 `compile()`。

ADR-024 把 Vulkan 定为 compile-review-only，这是项目级的证据政策，不是审查者否认的事实。看法是：证据政策可以是「本机不跑 Vulkan」；它不必是「Vulkan 翻译单元同时充当规格附录与残缺实现」。规格应在 ADR；实现文件应只含可执行路径或明确的 `Unsupported` 返回。

### 2.4 Metal 主文件违反了「一个翻译单元一个职责」

4364 行的 `.mm` 同时包含：

1. GPU 资源表与生命周期（`Impl`）；
2. 从 `ExecutionPlan` 到 `CompiledPlan` 的降低（`CompileOps`）；
3. 提交编排与多种执行模式（`SubmitOps`）；
4. 不经过计划对象的 facet / raster / cull 实验入口。

这是内聚性（cohesion）失败，不是单纯的「文件太长」。1k 行规则在这里是症状阈值：过线之后，下一次 `run_*` 或新 flavor 的边际成本是再搜一遍 4000 行文件，而不是打开一个 300 行的模块。`metal_tier2.mm` 已经证明团队知道可以把一块策略抽到独立翻译单元；主文件没有沿用同一纪律。

`submit` 里 raster 必须「软失败」再经 `finish` 合并，是因为后续 compute 路径会把 `submission->result.ok` 写成 true。这在控制流图上是两条本应独立的流水线（光栅编码 vs compute 模式选择）被串进同一个函数后的补偿逻辑。它能工作，但把不变量从数据类型挪到了「调用顺序与 lambda 关闭包」。新读者必须同时记住：raster 先跑、失败不立刻返回、`take` 可能提前返回、`finish` 负责回写。这是圈复杂度（cyclomatic complexity）以「正确的时序技巧」的形式出现。

公开的 `run_*_facet` 家族还造成了接口隔离问题（Interface Segregation）：依赖编译提交的调用方不需要这些方法，依赖垂直切片的测试却必须链接整个 `DeviceHal`。Metal 与 Vulkan 的同名操作签名不一致（`run_sample_facet` 重载数不同、raster 方法名不同、classification 结果类型不同），因此测试也无法对 `hal::DeviceHal` 写后端中立的 harness。

### 2.5 重复实现的是同一序列化合同，不是「两个后端长得像」

`pack_task_record` 在 Metal 与 Vulkan 各有一份，14 个字、同一位移、同一填充。这是同一 GPU 侧记录布局的两份编码器。布局注释指向 `compiler::task_ring_metal_source()` 的期望。真正的单一事实来源应在定义该布局的编译器（或 core 的 `TaskRecord` 序列化），而不是在两个 HAL 里手写平行函数。

两份编码器都不写 `task.kind`。Vulkan 因此必须在 `compile` 入口用循环拒绝 raster 任务。这是用控制流补丁掩盖数据模型缺口：任务的种类是 `TaskRecord` 的一部分，打包格式把它丢掉了，下游只能猜测。Metal 能跑 raster task graph，是因为 raster 走了 `SubmitOps::raster` 而不是这份 packing；Vulkan 没有对等的 submit 路径，于是 packing 的信息丢失变成了安全漏洞（静默 compute 伪装），再用拒绝循环堵住。同一布局合同，两种补救。

`FacetUseGuard`、texel decode、resolve_facet 的「status → error」模式同样平行存在。这不是偶然相似，是缺少一层「facet 操作的后端中立描述」。Reference 的 `sample_facet` / `raster_triangles` 已经是语义 oracle；GPU 路径各自发明了一套公开方法。双轨实现在一致性测试上有价值，但公开 API 形状不共享，oracle 与 GPU 之间不能做结构对结构的差分，只能做结果对结果的差分。

### 2.6 分层依赖的方向被局部政策改写了

预期的依赖方向是：`include`（C ABI）→ `src/api`（句柄与 marshaling）→ `src/core`（语义）→ `src/ir`；`src/backends` 依赖 core / compiler / ir；`src/api` 通过工厂取得 `DeviceHal`。

实际出现的反向或越层包括：

- `core.h` 前向声明 `hal::ExecutionPlan`，`ExecutionEnvelope::apply_to` 的定义放在 API 翻译单元，以便 `vg_core` 不链接 backends。这是用链接图约束掩盖了概念依赖：core 类型的方法以 HAL 计划对象为参数，语义上 core 已经认识 HAL。
- `submit()` 在 API 层做 `format_tag` 分派并调用 `ir::parse_module` / `parse_msl_raster_envelope`。解析属于 IR 语言前端。放在 ABI glue 里，意味着每增加一种 `format_tag` 都要改 C 入口，而不是改 `CodeObject` 的加载或 compiler 的调度。
- `create_device` 的 `switch (backend_kind)` 写在 `vg_api_device.cpp`，并直接包含 Metal / Vulkan 头。后端注册本应属于 `src/backends` 的 loader / probe。
- `VgAllocation` 绕过 `HandleRegistry`，用宿主指针充当句柄。其余 F1 对象都是 heap 上的包装结构加 registry。两种句柄代数并存：带世代的不透明令牌，以及可解引用的生指针。后者把 Arena 内部对象的稳定性暴露给 ABI 调用方，`is_live_allocation` 是事后补丁而不是类型系统保证。

知识图上的 `backends → api`、`core → api`、`core → backends` 与这些源文件观察一致。它们的数量不大，但方向错误；分层问题看的是方向，不是边的计数。

`core.h` 本身是领域模型的单一编译单元。Arena、Facet、TaskGraph、证书、信封、NodeTable 是可分离的 bounded context。挤在一个头里的代价是：任何只想用 `Arena` 的翻译单元都要解析 992 行与 `ir/ir.h`。PixelFormat / CanonicalView 的注释引用 Metal 文档——作为跨后端字节布局契约放在 core 可以成立，但它强化了「core 头 = 所有里程碑抽屉」的引力。

### 2.7 实验编排把「Vulkan 不跑」编码成了协议

`vg_exp.py` 的 Phase 字典与三段近乎同构的 `create_phase_*_run` 是经典的拷贝修改。更结构性的问题是：工具输出把 Vulkan 样本的状态写成固定的 compile-review-only。于是 C++ 侧「注释代替接线」永远不会被实验门拦住——门的定义已经把「不跑」当成合格。这是度量被实现污染（Goodhart）：证据系统测量的是「文档是否写了映射」，不是「第三个适配器是否走同一条提交管线」。

### 2.8 什么不是问题

下列事实不构成这次的阻断理由，避免把审查做成吹毛求疵。

- 公共 `vg.h` 保持单头、C 类型、函数表 append-only：对 ABI 稳定性是正确形状。行数 558 不是问题；问题是与 `core::TaskRecord` 镜像维护的负担，头文件自己已经披露。
- `hal::DeviceHal` 只保留三个虚函数：作为跨后端的稳定入口是对的。要改的是计划对象与具体类上的额外表面，不是把虚表加到二十个方法。
- 共享的 `run_representation_stage` 等把「语义簿记」与「物理变换」分开，是已经做对的抽象。问题是 Vulkan 不调用其中一部分，以及 Metal 在共享阶段之外还有第二套模式调度。
- 无 `TODO`、少 `reinterpret_cast`、无 `dynamic_cast`、无 `std::any`：局部编码卫生是好的。复杂度没有用这些标记标出来，所以不能靠搜 TODO 找到结构债。
- Reference 作为语义裁判、Metal 作为可执行 GPU 路径：项目定位如此。审查反对的是「第三个后端用同一乘积类型假装对等」。

### 2.9 综合

落地树上同时出现了：隐式乘积导致的特征组合爆炸、用控制流模拟和类型、翻译单元职责混杂、同一序列化合同的双份实现、分层依赖倒置、以及实验门对残缺实现的免责。它们互相加强：乘积越宽，每个 HAL 文件越长；文件越长，越不愿意抽共享管线；共享管线越不被第三后端使用，注释映射越显得「够用」。这是结构债的正反馈，不是三件无关的小事。

---

## 三、思路

这一节写打算如何改。原则是：先删除一类复杂度，再考虑搬家；行为在公开 C ABI 与已关门实验上保持不变；Vulkan 的「本机不跑」政策可以保留，但不能再以散文充当实现。

### 3.1 把一次提交的主意图从无标签记录改成有标签的数据

目标不是「再加几个 bool」，而是让非法组合在构造期或单一校验函数里不可表示。

具体做法是把当前 `ExecutionPlan` 拆成两部分。

**对所有模式都有意义、且彼此可交换的公共头：** ABI 版本、`CapabilitySnapshot`、timeline wait/signal、`published`、`graph_epoch`、`task_graph`（允许空）、`validation_profile`、以及 Stage 5 / working-set / discovery / envelope 这些已经由共享自由函数解释的输入。这些字段是真正的乘积：它们可以同时出现，语义正交（working-set 不改变 IR 形状，discovery 不改变 shader 源）。

**一次提交的主计算意图：** 用带标签的联合（C++ 的 `std::variant`，或手工的枚举 + 对应载荷结构体）表达当前实际上互斥或近乎互斥的那些路径，例如：

- 由 `ir::Module` 经 `build_linear_compute_package` 降低的线性 compute；
- 同一 IR 经 `build_indexed_compute_package` 降低的 indexed compute；
- 由 `effect_dag_passes` + 依赖边驱动的多 pass；
- 由 `user_raster_shader` 驱动、且任务仅为 raster 的受限 MSL 导入；
- 仅证书 / 仅 host-assisted、没有 GPU dispatch 的路径。

标签存在之后，`CompileOps::select_package` 不再对四个可选字段做 if-else 探测，而是对联合做穷尽匹配（`switch` + `default` 里对 `never` 的编译期检查，或 `std::visit`）。Metal `submit` 也不再对 `precheck_timeline` / `host_assisted` / `certificate` / `effect_dag` / `indexed` / `linear` 做顺序 `take`：主意图在 `compile` 结束时已经写入 `CompiledPlan`，`submit` 只执行那一种模式，光栅若仍是独立流水线，则作为公共头里的可选阶段，其成功/失败写入独立字段，而不是事后用 lambda 覆盖 `result.ok`。

`CapabilitySnapshot` 的用法随之变明确：校验函数的类型是「（公共头 × 主意图 × 能力位）→ 通过或 `Unsupported`」。未广告 `IndirectTier1` 却要求 Tier1、未广告 `UserShaderImport` 却携带 MSL 合同、Vulkan snapshot 却要求 raster 任务，都在进入任何后端的 `compile` 之前失败。后端实现不再负责「忽略无意义字段」。忽略从类型上消失。

迁移可以分步、保持线线兼容：第一阶段在 `ExecutionPlan` 旁增加联合字段，由现有 bool/vector 推导标签，并在 debug 断言两边一致；第二阶段让内部调用方只填联合；第三阶段删除旧字段。公共 C ABI 不必同步暴露联合——`vg_api_execution.cpp` 仍从 `VgSubmitDesc` 装配内部计划，只是装配目标变成「公共头 + 一个构造器」。

### 3.2 按职责切开 Metal 翻译单元，而不是继续在 `.mm` 里分段

`CompileOps` / `SubmitOps` 已经是逻辑边界，应升为翻译单元边界。

建议的文件集合（名字可再定，职责不行模糊）：

1. **资源与缓存。** 今日 `Impl`：buffer / texture / sampler / pipeline / task-ring 的确保、回收、facet 槽与 GPU 对象的对应。对外是窄的 C++ API，不谈 `ExecutionPlan`。
2. **降低（compile）。** 今日 `CompileOps`：在 3.1 的标签上选择 compiler 包、处理 representation 预检、时间线、管线对象附着。输出 `CompiledPlan`。
3. **提交编排。** 调用已有共享 stage，再调用资源层的 dispatch。光栅作为独立阶段，结果写入 `Submission` 的 `raster_results`，不与 compute 的 `result.ok` 抢同一个布尔。
4. **实验与垂直切片驱动。** 今日 `run_*_facet` / `run_cull_compact` / `run_pipeline_classification`。它们可以留在 `vg::metal` 命名空间，甚至留在测试目标里，但不应作为 `DeviceHal` 的成员。需要 facet 实验的测试直接构造 harness，而不是从 `hal::DeviceHal*` 向下转型。

公开的 `vg::metal::DeviceHal` 在这一步之后与基类对齐：三个虚函数 + 诊断用 `snapshot()`（若加载器仍需要）。测试若必须走 GPU facet，走 harness 类型，其坐标与结果结构与 `reference::SampleCoord` 等对齐，而不是再维持「故意不是同一类型以免头文件互相包含」的平行结构——平行结构可以留在 ABI 边界，内部测试应用同一描述。

`metal_tier2.mm` 保持独立。主文件切开时，Tier1 indirect、effect DAG dispatch 跟着提交编排或资源层走，看它们是否只依赖 `Impl` 状态。

### 3.3 让共享 stage 成为提交的骨架，后端只填物理回调

`run_representation_stage` 已经把「何时 stamp epoch、何时 consume」从后端拿走。应对 discovery、working-set、envelope 做同样的事，并且**由一个函数规定调用顺序**，而不是由每个后端的 `submit` 手写顺序。

形状可以是：一个 `run_submit_stages(plan, arena, device, physical, submission)`，内部顺序固定为例如：graph-epoch 检查 → discovery → working-set → representation（调用 `physical.transform`）→ 按 `CompiledPlan` 上的主意图 dispatch → envelope continuation（host 侧切分发布顺序，再交给 `physical.publish_tasks`）。Metal 与 Reference 今日已经接近这条顺序，差异在于 Metal 把 raster 插在 representation 与 compute 模式之间。把 raster 定义为公共骨架上的可选阶段之后，Reference 的 `raster_triangles` 与 Metal 的 raster pass 成为同一阶段的两个 `physical` 实现。

Vulkan 的义务变简单，也变硬：

- 若本机构建声明了某 `Capability` 位，则必须提供对应 `physical` 回调，或在校验期因能力位未广告而进不了该阶段；
- 若政策仍是 compile-review-only、本机永不执行，则能力位保持清零，`compile` 对需要该能力的意图返回 `Unsupported`，注释从 `.cpp` 移回 ADR；
- 禁止第三种状态：能力或字段看起来像支持，实现是注释。

`submit` 里手写 `deterministic_order` 而不调用 `apply_envelope_continuation` 的路径应删除，改为调用该 helper。否则三个后端会对 overflow / leftover token 产生分叉语义，跨后端差分会比较两种信封代数。

### 3.4 任务记录的 GPU 布局只保留一个编码器

把 `pack_task_record` / `unpack_task_record` 放到定义 task ring shader 布局的模块（`compiler` 中与 `kTaskRingWordsPerRecord`、`task_ring_metal_source` 相邻，或 `core` 中与 `TaskRecord` 相邻的序列化）。两个 HAL 只调用它。

同时决定 `TaskKind` 是否进入这 14 个字。两条路都完整，不能再「打包丢种类、入口再拦」：

- **种类进入布局：** shader 与 host 都看见 `kind`，错误的 dispatch 能在 GPU 侧守卫，或 host 在打包前断言 compute ring 只含 compute 任务；
- **种类不进入布局：** 类型上 task ring 的输入就不是 `TaskRecord`，而是一个不含 raster 字段的 compute-only 记录；raster 任务从类型上进不了这个编码器。Vulkan 的 for-loop 拒绝成为多余，因为非法值构造不出来。

我倾向第二条在 C++ API 上更干净（ring 的定义域变小），第一条在调试 dump 上更方便。无论哪条，单一编码器是前提。

### 3.5 把越层的职责送回原层，且不靠「定义放在别的 .cpp」来假装 core 不依赖 HAL

- **IR 与格式分派。** `CodeObject` 加载时（或第一次 `submit` 前的明确 `materialize`）完成 `format_tag` → 解析。`vg_api_execution.cpp` 只把已经解析的 `ir::Module` 或 `UserRasterShaderContract` 放进计划。新增格式改 `src/ir` 或 `src/compiler`，不改 C 入口。
- **后端工厂。** `src/backends` 提供 `make_device_hal(BackendKind, uuid, error)`，内部再分发 Metal / Vulkan / Reference，并用同一组 `VG_HAS_*`。`vg_api_device.cpp` 只调这一入口。API 翻译单元不再包含 `metal_device_hal.h`。
- **信封应用到计划。** 不要让 `core::ExecutionEnvelope` 的方法接受 `hal::ExecutionPlan&`。改成自由函数，放在已经同时依赖 core 与 hal 的翻译单元（例如 `device_hal.cpp` 或新建的 `src/backends/plan_assembly.cpp`）：`void apply_envelope(const core::ExecutionEnvelope&, ExecutionPlan&)`。`core.h` 删除对 `hal::ExecutionPlan` 的前向声明。链接图与概念图重新对齐：core 不知道 hal。
- **分配句柄。** `VgAllocation` 进入与其它 F1 对象相同的 `HandleRegistry` 包装（持有 `Arena*` + id/generation，或持有 `Allocation` 的非拥有令牌）。去掉 `reinterpret_cast`。失效句柄在 registry 查询失败时失败，而不是在解引用后靠 `is_live_allocation` 补救。这关闭的是句柄代数的不一致性，不只是一次类型转换。

### 3.6 按有界上下文切开 `core.h`，shader 字符串离开 `compute_package.cpp`

`core.h` 不需要一次拆成微服务，但应拆成能独立包含的头：`arena.h`、`facet.h`、`task_graph.h`、`certificate.h`、`envelope.h`、`node_table.h`，由 `core.h` 做聚合以便旧包含点继续工作。实现 `.cpp` 按同样边界拆，避免 1510 行单文件。这是编译隔离与可读性，不改变语义。

`compute_package.cpp` 里的 MSL/GLSL 字符串迁到独立的 `shader_templates` 翻译单元或资源。包构建器只做 IR → binding 表 → 选择模板。facet shader 的修改不应打开 639 行的包逻辑。

`reference_executor.cpp` 在触及 1k 行之前按算法边界切开：解释器、facet oracle、软件光栅。它们已经是三段独立算法，只是住在同一文件。

### 3.7 实验 runner 抽成数据驱动的一轮循环

`vg_exp.py` 用一份「phase → 实验 id → ctest 名」的数据，加上一个 `run_phase(definitions, experiment_map, report_title)`。Phase C/D/E 的 130–140 行拷贝变成该函数的三次调用。Vulkan 的 compile-review-only 若仍是政策，应来自定义文件里的 `evidence: compile-review-only` 字段，而不是写死在 report 字符串里。这样，一旦 Vulkan 开始走共享提交骨架，实验定义可以改字段，而不改五份 runner。

这一项不阻塞 C++ 结构，但阻止证据系统继续给 2.3 的分叉实现发合格证。

### 3.8 顺序与明确不做什么

建议的落地顺序按「删除的复杂度 / 引入的风险」：

1. 抽出 `pack_task_record` 的单一实现，并写一个两边布局一致的单测。行为零变化，风险低，立刻去掉一类双写。
2. 引入提交主意图的标签（由现有字段推导），让 Metal `submit` 按标签分派，删除顺序 `take` 探测；raster 结果与 compute 结果分字段存储。对外 ABI 不变。
3. 规定共享 stage 的调用顺序，Metal / Reference 改为调用该骨架；Vulkan 要么接入，要么清掉相应能力位并删除反事实注释。
4. 切开 `metal_device_hal.mm` 的四个职责。这是机械移动加包含调整，应在 2、3 之后，以免先搬家再改控制流。
5. API 层的解析、工厂、句柄、`apply_to` 归位。
6. `core.h` / `compute_package.cpp` / `vg_exp.py` 的拆分。

在 1–3 完成之前，不应再向 `ExecutionPlan` 增加布尔或 `optional` 字段，也不应再向 `metal_device_hal.mm` / `vulkan_device_hal.cpp` 增加与新里程碑对应的公开 `run_*` 或新的 `#if` 注释块。新功能应表现为：一个新的主意图构造器，或一个新的共享 stage，或一个新的 `physical` 回调。若功能无法这样表达，说明它还没有被理解成与现有管线正交的阶段，应先改数据模型，而不是先改 HAL 文件。

不在本次思路里做的事：重写公共 C ABI、合并 Metal 与 Vulkan 的 GPU API、删除 Reference oracle、为了「好看」把 558 行的 `vg.h` 拆成多头。那些有各自的 ABI 与证据成本，与这次审查的结构债不是同一问题。

---

## 附录：与 skill 输出顺序的对照

| Rubric 优先级 | 落在本文 |
|---|---|
| 结构债 / 结构回归 | 2.2、2.3、2.4 |
| 可见却未做的大幅度简化 | 3.1–3.4（共享 stage 已存在、packing 已重复、Tier2 已独立成文件） |
| 控制流上的特判增长 | 1.5 的 `take` 链、1.6 的 raster 循环、2.2 的特征交互 |
| 边界与类型契约 | 2.5、2.6、3.5 |
| 文件体量 | 1.2、2.4、3.2、3.6 |
| 模块性 | 2.3、2.4、3.3 |
| 可读性 | 1.6 的注释映射、2.3、2.7 |

按该 skill 的通过门槛，第一次快照不能批准为「可以在此结构上继续加功能」的基线。批准条件是 3.8 中 1–3 落地，使得下一次里程碑有一个不等于「再给乘积类型加一维」的插入点。

F6 随后落地：它**没有**给 `ExecutionPlan` 增加布尔或 `optional` 字段，也没有给 `DeviceHal` 增加新的 `run_*`。这满足 3.8 对「插入形状」的约束。第二次审查要问的是：合同形状对了之后，接线、测试与分层是否仍然过关，以及把范围扩到整仓之后还看到了什么。

---

## 第二次审查（2026-08-26）：F6 差量 + 整仓

- 审查对象：工作区相对 `b6bd6db` 的未提交差量（F6 SceneRoot，见 ADR-052），以及同一工作区上的整个 git 跟踪树（`src/`、`include/`、`tools/`、`tests/`、`CMakeLists.txt`、`schemas/`、`docs/` 中与代码合同相关的部分）。
- `git diff --stat`（已跟踪文件）：12 个文件，+218 / −21。另有未跟踪：`docs/decisions/ADR-052-f6-scene-root.md`、`include/vg/vg_scene_root.h`、`src/core/scene_root.{h,cpp}`、`tests/api/vg_f6_scene_root.c`、`tools/vg-offscreen-triangle-ppm.c`、`schemas/ir/scene-root-raster.vg.json`、仓库根上的 `offscreen_triangle.ppm`。
- 核对过的新增/改动文件：ADR-052、`scene_root.{h,cpp}`、`vg_scene_root.h`、schema JSON、`device_hal.cpp` 的 `validate`、`metal_device_hal.mm` 的 raster 绑定与 identity buffer、`reference_device_hal.cpp` 的 CPU 变换、`compute_package.cpp` 的 MSL 声明、`vg_api.cpp` 版本链、`CMakeLists.txt` schema 生成、`capture.cpp` 拒绝、`vg_f6_scene_root.c`、`vg-offscreen-triangle-ppm.c`、`04-public-c-abi.md` 示例段。
- 整仓新纳入的体量对象：四个超过或逼近 1k 行的测试翻译单元、CMake 的 per-test 可执行文件增长、文档示例与落地 ABI 的差异。
- 裁决分两条，可以不同：（a）F6 差量作为里程碑；（b）整树作为 F8 窗口化 / 呈现的健康基线。见 6.9。

---

## 四、事实（第二次）

这一节只记录可复核的观察。先写 F6 差量实际做了什么，再写第一次审查范围之外、整仓里实际长什么样。

### 4.1 审查过程

过程仍按 skill 编排：先 `git status` / `git diff --stat` / 全仓库 `git ls-files` + `wc -l`；再并行阅读 F6 新文件与 Metal/Reference/validate 热路径；再用 explore 子代理覆盖 `tests/` 与 CMake；最后把差量与整仓证据交给 `thermo-nuclear-code-quality-review` 子代理，父代理回读行号。

与第一次的差异：当时 `git diff main` 为空，对象是三棵目录的快照；这次对象是未提交工作树 + 整仓。第一次明确「未审查 tests/ 与 docs 正文」；这次把测试树和 `04-public-c-abi.md` 的示例段纳入事实。

未做：没有重新跑 ctest（调用方已声明 F6 测试通过）；没有运行时剖析；没有对每一个小于 100 行的测试文件做语句级走读。

### 4.2 F6 合同在 ADR 与代码里实际如何对齐

ADR-052 的决策与代码一致的部分：

- 发布 header/API **v1.7**，**不**增加 `VgApi` 函数指针、**不**增加 task-record 布局、**不**增加 UBO / 常量表资源族。
- 调用方用已有的 `VgTaskRecordV2.root` / `root_generation` 命名一块稳定分配，用 F7 的 `writeAllocation` 在每次 submit 前改 SceneRoot 字节。密封后的任务图可跨帧复用。
- 布局的单一 JSON 源是 `schemas/ir/scene-root-raster.vg.json`：列主序仿射矩阵 `camera_clip_from_local[16]`，以及 `Material { base_color[4], albedo: facet_ref }`。生成器发出公共 C 头、`sizeof`/`offsetof` 断言、schema id、reflection、以及 `material.albedo` 相对根的 relocation 偏移 80。
- 启用条件是 CodeObject / 模块或受限 MSL envelope 的 `root_schema` **精确等于** 生成的合同名 `vg.scene-root.raster/v1`（宏 `VG_SCHEMA_SCENEROOTRASTER_CONTRACT_NAME`）。
- SceneRoot 任务必须：`raster_facets.source` 为空（`material.albedo` 为唯一采样源）；`raster_tint` 为恒等（`material.base_color` 为唯一 tint）；相机 `w` 分量为仿射（第 3、7、11 分量为 0，第 15 分量为 1）；变换后深度有限且落在 `[0,1]`。顶点 / index / color / depth facet 仍用任务记录上的既有字段。
- 混合 compute+raster **仍推迟**。`ExecutionPlan::validate` 对 SceneRoot schema 再扫一遍任务种类。
- Capture v1 不能对嵌在 root 字节里的 `FacetRef` 做 snapshot/reacquire；`capture.cpp` 在 `is_scene_root_raster_schema` 时返回明确错误，而不是重放过期 token。

ADR-052 声称 Reference 与 Metal 共用同一 resolver。代码上 `core::resolve_scene_root_raster` 确实被两边调用。顶点上的相机变换则**不是**同一条实现，见 4.5。

### 4.3 F6 新增与修改的文件（体量）

| 路径 | 行数（约） | 角色 |
|---|---|---|
| `src/core/scene_root.h` | 31 | `ResolvedSceneRootRaster`；`is_scene_root_raster_schema` / `resolve_scene_root_raster` / `transform_scene_root_vertex` |
| `src/core/scene_root.cpp` | 81 | 解析、仿射检查、列主序矩阵乘 |
| `include/vg/vg_scene_root.h` | 33 | 生成头，check-in 到公共 include；`#include <vg/vg.h>` |
| `schemas/ir/scene-root-raster.vg.json` | 24 | 布局源 |
| `tests/api/vg_f6_scene_root.c` | 88 | 仅公共头的 C ABI 测试：同一密封图 submit 两次，红然后绿，再平移相机确认变换生效 |
| `tools/vg-offscreen-triangle-ppm.c` | 205 | 独立样例；请求 **v1.6**，走 legacy `raster_facets.source`，不是 SceneRoot |
| `docs/decisions/ADR-052-f6-scene-root.md` | 57 | 决策记录 |

已跟踪文件上的净增：`metal_device_hal.mm` 4364→4414（+50）；`reference_device_hal.cpp` +35；`device_hal.cpp` +11；`vg_api.cpp` 版本链扩展；`compute_package.cpp` 顶点 shader 改为读 slot 1 的 SceneRoot；`CMakeLists.txt` 第二套 schema `add_custom_command`、`vg_core` 增加 `scene_root.cpp` 与 `PUBLIC include/`、两个新可执行文件（F6 测试与 PPM 样例）；`capture.cpp` 拒绝 SceneRoot replay；`vg_schema.py` 增加 `facet_ref` 标量、relocation 收集、可配置 `output` 文件名。

F6 **没有**把任何原本低于 1000 行的文件推过 1000 行。新生产文件都远小于该阈值。

### 4.4 `vg_core` 与公共头的实际依赖

`CMakeLists.txt` 在 F6 之后：

- `vg_core` 源文件增加 `src/core/scene_root.cpp`；
- `target_include_directories(vg_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")`（此前 core 只需要 `src/` 与生成目录）。

`scene_root.cpp` 包含生成头 `vg_scene_root.h`。该头第 7 行 `#include <vg/vg.h>`，因为 `facet_ref` 被映射为 `VgFacetRef`。于是编译 `vg_core` 会解析完整的公共 C ABI 头（句柄、`VgApi` 表、版本宏）。`task-root` 生成物仍只出现在构建目录 `${VG_GENERATED_DIR}`，**没有** check-in 到 `include/vg/`。SceneRoot 是目前唯一被放进公共 include 树的生成布局。

`core.h` 里已有 `struct FacetRef { uint32_t index; uint32_t generation; }`。生成头里的 `VgFacetRef` 是同一 8 字节布局的 C 名字。

### 4.5 Metal 与 Reference 在 SceneRoot 上实际做什么

**共享：** `SubmitOps::raster`（`metal_device_hal.mm:2766` 一带）与 Reference `submit` 的 raster 段都根据

```text
root_schema = user_raster_shader ? user_raster_shader->root_schema : module.root_schema
uses_scene_root = core::is_scene_root_raster_schema(root_schema)
```

决定是否调用 `resolve_scene_root_raster`。解析成功后两边都把 `facets.source` 换成 `root.albedo`、把 tint 换成 `root.base_color`。

**Metal 独有：**

- 内置 MSL 顶点阶段现在无条件声明 `constant VgRasterSceneRoot& root [[buffer(1)]]`，并用列构造 `float4x4` 乘顶点（`compute_package.cpp` 约 488–507 行）。注释要求不要用 `float4x4` 作为结构体成员，以免 16 字节对齐把 88 字节 host 根撑开。
- 非 SceneRoot 的 raster 任务调用 `Impl::make_identity_scene_root_buffer()`：分配一块 Shared buffer，尺寸为 `16 * sizeof(float) + 4 * sizeof(float) + 2 * sizeof(uint32_t)`，把对角置 1。`run_raster_triangles`（约 4069 行）同样分配一块 identity root。每次这样的 draw 都 `newBufferWithLength`，未见设备级缓存。
- SceneRoot 路径上，CPU 用 `transform_scene_root_vertex` **校验**变换后深度，循环变量是局部 `RasterVertex vertex`；随后 GPU shader 再乘一次矩阵。CPU 不改写即将绑定的 vertex buffer。

**Reference 独有：**

- 在 `uses_scene_root` 时，对每个顶点先检查 F4 的源 `z ∈ [0,1]`，再调用 `transform_scene_root_vertex` **写回** `vertex.x/y/z`，然后把变换后的数组交给 `raster_triangles`。软件光栅看到的已经是 clip 空间位置。

因此：合同（仿射相机 + 深度范围）一份；Metal 是「CPU 守卫 + GPU 变换」；Reference 是「CPU 变换后光栅」。两边都调用同一个 `transform` 函数，但只有 Reference 把函数输出当作光栅输入。

Vulkan：`vulkan_device_hal.cpp` 无 SceneRoot 符号。`compile` 仍对 `TaskKind::Raster` 填 `Unsupported`（约 2156–2168 行）。`raster_facet_vulkan_source()` 仍是旧的 xy 直出、无 SceneRoot 绑定。

### 4.6 `ExecutionPlan::validate` 里实际有两段「必须全是 Raster」循环

`device_hal.cpp:106-132`：

1. `user_raster_shader.has_value()` 时，要求 `vertex_abi` 匹配，并遍历任务，非 Raster 则失败（F3 既有）。
2. 然后无论是否 user shader，取 `root_schema`；若 `is_scene_root_raster_schema`，再遍历一遍任务，非 Raster 则失败，诊断字符串不同（「compute+raster mixing is deferred」）。

两段谓词相同（`task.kind != Raster`），触发条件可重叠（user MSL 的 envelope 也可以带 SceneRoot 合同名）。

### 4.7 版本协商在 `vgGetApi` 里实际如何增长

v1.7 **没有**新的 `VgApi` 成员。`writeAllocation` / `readAllocation` 仍挂在 `at_least_v1_6` 上；该布尔现在是 `requested_version == 1_6 || == 1_7`。

允许的版本是从 1_0 到 1_7 的一串 `!=` 否定。`at_least_v1_1` … `at_least_v1_4` 各是一条把所有更新版本用 `||` 连起来的等式链。每增加一个「无新指针的头版本」，这些链各加一项。`sizeof(VgApi)` 边界：v1.7 与 v1.6 相同（ADR-052 也这么写）。

### 4.8 F6 测试与样例实际覆盖什么

`tests/api/vg_f6_scene_root.c`：

- 只包含 `<vg/vg.h>` 与 `<vg/vg_scene_root.h>`。
- `vgGetApi(VG_API_VERSION_1_7, …)`。
- 枚举适配器后 **优先挑选 `VG_BACKEND_METAL`**（第 44 行）；本机有 Metal 时不会走 Reference。
- CodeObject 是 `snprintf` 拼出的 JSON：`root_schema` 为合同名，instructions 里一条对 sample allocation 的 `load`。这条 load 不是 SceneRoot 语义的一部分，作用是让 `ir::verify` 接受模块。
- 同一 `VgSubmitDesc`（同一 graph + envelope）submit 三次：红 tint、绿 tint、再把相机平移分量写成 4 以确认像素不再是绿。
- 不覆盖：仿射失败、非有限矩阵、非空 `source` facet、非恒等 tint、过短 allocation、capture 拒绝、Reference 后端、Vulkan 拒绝。

`tools/vg-offscreen-triangle-ppm.c` 请求 **v1.6**，root_schema 为 `vg.example.offscreen-triangle/v1`，任务带 legacy `raster_facets.source`。默认把 PPM 写到 `offscreen_triangle.ppm`（cwd）。该文件当前未跟踪，也未出现在 `.gitignore` 的已读片段中。

`tests/unit/core_test.cpp`（1401 行）、`tests/unit/reference_raster_test.cpp`（1027 行）、`tests/api/vg_c_abi_conformance_test.cpp`（1309 行）、`tests/vertical_slice/metal_task_timeline_test.cpp`（3337 行）在本次差量中 **没有** SceneRoot / v1.7 用例。vertical slice 的 `basic-raster` 模式调用 `run_raster_triangles`，不经过 `compile`/`submit`。

### 4.9 整仓测试树与构建图（第一次未计入）

全仓库 `git ls-files` 约 296 个跟踪路径。按行数，生产代码之外新进入审查的超大文件：

| 文件 | 行数 | 结构（打开后的观察） |
|---|---|---|
| `tests/vertical_slice/metal_task_timeline_test.cpp` | 3337 | 单 `main`，argv 分派约 19 个模式；部分走 `compile`/`submit`，部分直连 `run_*_facet` / `run_raster_triangles` / `run_cull_compact` |
| `tests/unit/core_test.cpp` | 1401 | 单 `main` + `assert` 串接 Arena / TaskGraph / ExecutionPlan / Facet / Phase C / D1 |
| `tests/api/vg_c_abi_conformance_test.cpp` | 1309 | 仅 `vg.h`；v1.1 golden path、句柄失效、version skew、F3.5 光栅、F5 indexed；无 F6 |
| `tests/unit/reference_raster_test.cpp` | 1027 | 直连 `sample_facet` / `raster_triangles` oracle，后半段才 `DeviceHal::submit`；无 SceneRoot |

`CMakeLists.txt` 约 378 行：约 29 个 `add_executable`、约 51 个 `add_test`；Metal 开启时再为 vertical slice 登记约 18 个 ctest 名，共用上述 3337 行二进制。Phase D/E 通过 `cmake/phase-d-*.cmake` 等 glob include 继续加测试。schema 生成：两段几乎镜像的 `add_custom_command`（task-root 与 scene-root）；`tests/tools/test_schema_generator.py` 再直接调两次 `vg_schema.py`。

`docs/vg-project/04-public-c-abi.md` 约 503–510 行仍展示 `sizeof(SceneRoot)`、`vgMakeSceneRoot(...)`、`upload_scene_root(...)`、`VG_SCHEMA_ID_SCENE_ROOT`。仓库内不存在这些符号。落地合同是 `VgSchema_SceneRootRaster`、`writeAllocation`、`VG_SCHEMA_SCENEROOTRASTER_CONTRACT_NAME`。`05-compiler-language-ir.md` 的 `struct SceneRoot` / `draw_scene` 是语言层愿景，不是当前 C ABI。

### 4.10 第一次审查指出的结构，F6 之后是否仍在

下列第一次记录的事实在 F6 差量中 **未被删除或改形状**：

- `ExecutionPlan` 仍是同一无标签记录（`device_hal.h:137-241`），F6 未增加字段。
- `metal_device_hal.mm` 仍是 Impl + CompileOps + SubmitOps + 公开 `run_*` 四合一，行数从 4364 增至 4414。
- Vulkan 仍是 stub+实现双编译、`pack_task_record` 双份、raster compile 拒绝。
- `vg_api_execution.cpp` 仍在 submit 入口解析 IR / `"vg.msl.raster/v1"`。
- `VgAllocation` 仍是 `reinterpret_cast` 生指针。
- `core.h` / `core.cpp` 行数未拆。

F6 **遵守**了第一次 3.8「不要再给 ExecutionPlan 加布尔、不要加 `run_*`」的插入约束。新特判出现在 `validate` 的第二段循环、`SubmitOps::raster` 的 if/else、Reference 的 `uses_scene_root` 分支、以及 `vgGetApi` 的版本 OR 链。

---

## 五、看法（第二次）

### 5.1 F6 合同形状与第一次思路的关系

第一次 3.8 要求新功能表现为既有管线上的数据，而不是计划对象上的新维。F6 把启用条件放在 `root_schema` 字符串上，把每帧可变状态放进 Arena 字节，用已有 `root_allocation` 当身份。这是正确的插入形状：一次提交的主计算意图没有因此多一个 `bool request_scene_root`。共享 `resolve_scene_root_raster`、capture 明确拒绝、生成 schema 做 host 布局源，也符合「语义在 core、物理在后端、不要静默降级」的方向。

否决不针对这个形状。否决针对的是：形状对了之后，接线把分层、布局单一事实来源、以及「谁拥有 raster 合同的测试」又拧了一层。

### 5.2 生成头把公共 ABI 灌进 core，是分层反转而不是生成器的必然代价

`FacetRef` 作为 8 字节能力令牌已经属于 core。生成器为了让 C 调用者写 `root.material.albedo` 时用 `VgFacetRef`，选择 `#include <vg/vg.h>`。公共头需要 `VgFacetRef` 说得通；**core 翻译单元**不需要因此解析 `VgApi`。正确边界是：生成布局里 `facet_ref` 发出本地 `{ uint32_t index; uint32_t generation; }`，公共头再 `static_assert` 与 `VgFacetRef` 同布局、同偏移。core 只 memcpy 生成结构，CMake 不必 `PUBLIC include/`。

`task-root` 生成物留在构建目录、`scene-root` check-in 到 `include/vg/`，是第二种不对称：并非所有生成布局都是公共 ABI。F6 需要应用可 `#include <vg/vg_scene_root.h>`，这个目标成立；实现时把 `vg.h` 一并导入 core 的编译图，是把「给应用看的头」和「给 vg_core 看的布局」做成了同一文件。

### 5.3 88 字节布局有一份 JSON 源，却有三份（加手算则四份）消费者

JSON → 生成 C 是机械的，且 schema 测试比较「生成头 == check-in 的公共头」，这一段是闭合的。断裂在：

- MSL 里的 `VgRasterSceneRoot` 是 `compute_package.cpp` 中的字符串。schema 增字段时 C 头会变、静态断言会红；MSL 不会编译失败，只会在运行时按错误步长读 slot 1。
- identity buffer 的长度是手写算术，碰巧等于 88。同一 `.mm` 已经能看见 `scene_root` 头，却不用 `sizeof`。这等于 Metal 热路径不信任自己刚引入的类型。
- Vulkan 的 raster GLSL 仍是旧顶点合同。F6 把「Metal 真路径 / Vulkan 文档路径」在 raster 上又拉开一档：现在连内置 shader 的输入槽集合都不一样。

这是第一次「`pack_task_record` 双写」同一类问题：合同有名字，编码器有多份。F6 在 host 侧用生成器做对了，在 GPU 侧和 identity 填充上没有接到生成器。

### 5.4 测试把 SceneRoot 放在 ABI 冒烟层，没有放进拥有光栅语义的层

`reference_raster_test.cpp` 与 `metal_task_timeline_test.cpp` 的 `task-graph-raster` 才是 raster 合同的回归所在：仿射拒绝、深度范围、source/tint 权威、submit 绑定。F6 只加了 88 行 C 测试，且优先 Metal。于是：

- Reference 的「变换顶点再光栅」在本机有 Metal 时不被这条测试执行；
- Metal vertical slice 的 raster 模式仍走 `run_raster_triangles`（identity root 那条），测不到 slot 1 与跨 submit 的 root 更新；
- 两套变换路径没有共同 oracle，浮点或深度守卫不一致时没有自动失败。

这不是「测试太少」的数量问题，是**合同的所有者写在错误的翻译单元**。ABI 冒烟证明公共头能画红绿三角形，不能证明 resolver、仿射子集、capture 拒绝、以及双后端变换同构。

Dummy `load` 指令说明 CodeObject 仍是「必须能过 `ir::verify` 的 IR 信封」，而不是「带 root_schema 的根」。启用机制挂在 schema 名上是对的；把假 instruction 当载体，使合同比 ADR-052 写的更窄：没有一条合法 load，SceneRoot 提交在 verify 就会失败。

### 5.5 特判从计划字段挪到了忙路径上的 schema 开关

F6 没有扩大 `ExecutionPlan` 的笛卡尔积，这是真简化。复杂度改出现在：`validate` 第二段循环、`SubmitOps::raster` 的 if/else、Reference 的 `uses_scene_root`、`vgGetApi` 的 OR 链。这是同一特征交互问题的平移：启用已经有规范名字，却在每个后端各写一段开关，而不是「一次解析、一种绑定策略」。

`validate` 两段 raster-only 循环是以后混合提交（ADR-047 已把 F6/F9 列为重开触发器）会再复制第三次的写法。F6 自己推迟了混合提交，却用复制循环把推迟实现成了两套诊断。

v1.7 零新指针却把版本矩阵扩一维，是差量里最不该存在的改动。版本号是全序；写成不等式或边界表后，无新成员的头版本不应触摸那六条链。

每 draw `newBufferWithLength` 一块 identity root，是用分配换「旧 PSO 在 slot 1 上永远合法」。设备级常驻 identity buffer、或 legacy 与 SceneRoot 两套 PSO，都能避免在 2800 行 submit 热路径上分配。当前写法把 ABI 兼容成本做成了每帧堆分配。

### 5.6 整仓测试与构建是与 HAL 平行的第二套结构债

第一次没看 `tests/`，所以 3337 行的 `metal_task_timeline_test.cpp` 没有进入 1k 规则的表。它比 `core.cpp` 更大，且把 19 个实验模式、两条执行轨道（submit vs `run_*_facet`）压进一个 argv 分派器。F6 没有加第 20 个模式，因此 SceneRoot 不在这套回归里——这既避免了文件再涨，也让最重的 raster 回归测不到 F6 改过的绑定。

CMake 每测一个 ABI 里程碑就 `add_executable` + `add_test`。schema 生成命令整段复制。这与 `vg_exp.py` 的 Phase runner 拷贝是同一增长函数：新实体 = 新一份样板，而不是列表上的新元素。

`04-public-c-abi.md` 仍在教 `vgMakeSceneRoot`。规范文档与落地 ABI 分叉时，集成方会按不存在的 API 写代码。这不是文风问题，是合同有两个互相矛盾的文本。

### 5.7 什么不是第二次的新阻断

- F6 测试能画三角形、跨 submit 改 root 颜色与相机：行为证据成立，不在这次否决范围内。
- 不增加 `VgApi` 成员、不增加 task-record、不用 UBO 族：ABI 纪律成立。
- `ResolvedSceneRootRaster` 保持数据子集、facet 获取仍走既有 raster 路径：抽象边界在 resolver 这一侧是对的。
- Capture 明确拒绝：诚实边界，不是静默重放。
- 新文件本身远小于 1k 行。
- PPM 样例走 v1.6 legacy：可以作为「F3–F5 路径仍可用」的样例，只要文档标明；问题是它被放在「offscreen triangle」这个与 F6 端到端叙事易混的名字下，且默认产出未忽略的工件。

### 5.8 综合

F6 证明：可以在不扩大 `ExecutionPlan` 积类型的前提下加入每帧场景根。整仓同时证明：测试树与 Metal 主文件仍是功能的吸引子；F6 把特判写进 submit 热路径和 validate 复制循环，把公共 ABI 头写进 core 的编译图，把 88 字节布局写成 JSON + C + MSL + 手算。合同进步了，接线仍低于门槛。F8 若在此基线上加窗口，最可能的落点仍是 `ExecutionPlan` 新 optional、`.mm` 第 4500 行附近的 if、以及 vertical slice 第 20 个 argv。

---

## 六、思路（第二次，在第一次 3.x 之上）

第一次 3.1–3.8 仍然适用，不在此重复。下面只写 **F6 差量使顺序变化之后** 应先做的事，以及整仓新暴露的测试/文档债。原则不变：先删除一类双写或一层反转，再搬家；F6 的公开 ABI 行为（同一密封图、改 root 字节、红/绿/相机）保持为回归锁。

### 6.1 切断 `vg_core` 对 `include/vg/vg.h` 的编译依赖

生成器对 `facet_ref` 发出本地 8 字节结构体（或生成一份不含 `#include <vg/vg.h>` 的 `vg_scene_root_layout.h` 给 core 用）。公共 `vg_scene_root.h` 再包含 `vg.h` 并把字段类型写成 `VgFacetRef`，用 `static_assert` 钉死与布局头同大小、同偏移。`CMakeLists.txt` 删除 `vg_core` 的 `PUBLIC include/`。core 继续 `memcpy` 生成布局，解析结果写入 `core::FacetRef`。

这是 F6 差量里唯一的分层回归，应在任何 F8 之前单独合入。

### 6.2 让 GPU 声明和 identity 缓冲消费同一 `sizeof`

`make_identity_scene_root_buffer` 使用 `sizeof(VgSchema_SceneRootRaster)`（或 core 导出、并在 `.cpp` 对生成类型 `static_assert` 的 `kSceneRootRasterBytes`）。MSL 的 `VgRasterSceneRoot` 必须有同大小的机械约束：由 schema 生成 MSL 片段，或在编译期对 packed 布局做与 C 头比对的测试（生成两份 blob，`memcmp`）。禁止只靠注释维持 byte-identical。

CMake 用一条函数/列表驱动 schema 文件，而不是复制 `add_custom_command`。`test_schema_generator.py` 对同一列表迭代，不再内联第二次调用。

Identity root 提升为 `Impl` 上的设备级常驻 buffer（惰性创建一次），或让 legacy raster 与 SceneRoot 使用不同的 PSO / shader 变体，使旧路径不必绑定 88 字节零矩阵。不要在每次 `SubmitOps::raster` 与每次 `run_raster_triangles` 上 `newBufferWithLength`。

### 6.3 合并 raster-only 校验与版本边界，不要为每个特性复制循环

`ExecutionPlan::validate` 抽「任务图是否全是 Raster」一次。`user_raster_shader` 与 SceneRoot schema 都只是触发该谓词的条件；诊断可以带原因字符串，谓词不能复制。以后若重开混合提交，只改这一处。

`vgGetApi` 把允许版本写成区间或表：`requested >= 1_0 && requested <= 1_7`，`at_least_v1_n` 写成 `requested >= 1_n`（在版本号编码为可比较整数的前提下——当前 `0x0001000Nu` 已满足）。v1.7 这种「头版本、函数表不变」的里程碑不应再改 OR 链。

### 6.4 把 SceneRoot 放进已经拥有光栅语义的测试，并固定双后端变换合同

最低回归集：

- `reference_raster_test.cpp`：resolve 成功/失败（仿射、非有限、source 非空、tint 非恒等、allocation 过短）；CPU `transform_scene_root_vertex` 的深度拒绝；submit 路径上 albedo/tint 权威。
- `metal_task_timeline_test.cpp` 的 **task-graph-raster**（`compile`/`submit`，不是 `run_raster_triangles`）：slot 1 绑定、跨 submit 改 root 字节。不要加第 20 个无关 argv；加在已有 raster-submit 模式里。
- `vg_f6_scene_root.c`：对 Reference 跑一遍，或在 CMake 里为两个后端各注册一次；不要只挑 Metal。
- `vg_c_abi_conformance_test.cpp` 至少承认 v1.7 与 1.6 同表大小，避免 version skew 矩阵漏一档。

变换合同：把「仿射 + 深度范围」留在 `resolve` / `transform`。Metal 热路径不要为了守卫再乘一遍又丢掉结果——要么只检查矩阵字段（已在 resolve 里做过），要么与 Reference 一样把变换定义为 CPU 职责（会改变 GPU shader，需显式 ADR）。当前双路径若保留，必须有对比测试：同一 root、同一顶点，Reference 输出像素与 Metal 在容差内一致。

### 6.5 文档与样例跟落地 ABI 对齐

改写 `04-public-c-abi.md` 示例：`#include <vg/vg_scene_root.h>`、`VgSchema_SceneRootRaster`、`writeAllocation`、`VG_SCHEMA_SCENEROOTRASTER_CONTRACT_NAME`、`createArena(device, …)`、`loadCodeObject` 的描述符形式。删除或标注 `vgMakeSceneRoot` 为未实现愿景。

`vg-offscreen-triangle-ppm.c`：要么升到 v1.7 SceneRoot（真正的端到端样例），要么在文件头与 ADR 标明「F3–F5 legacy 光栅，非 F6」。默认输出路径不要落在仓库根；写入构建目录或临时目录，并把 `*.ppm` 加入 gitignore。

### 6.6 整仓测试树：先停止向 monolith 追加，再按执行轨道切开

不要求立刻把 3337 行文件拆完。要求是：**新的 raster/ABI 行为不要只加在新的 88 行可执行文件里然后离开拥有合同的 monolith。** 切开的自然边界已经存在：

- 走 `hal::DeviceHal::compile/submit` 的模式 vs 走 `run_*_facet` 的模式（后者应迁出 `DeviceHal` 公开面，与第一次 3.2 一致）；
- `core_test.cpp` 按注释块拆 `facet` / `execution_plan` / `certificate`；
- 共享的 `fill_subresource` / `make_rgba8_view` 进 `tests/support/`，供 Metal vertical slice 与 reference raster 共用。

CMake：新 schema 只往列表里加一项；新 ABI 冒烟可以继续独立可执行文件（C 语言、只链 `vg_api` 有价值），但 conformance 应包含该版本号，避免第三份平行矩阵。

### 6.7 与第一次 3.8 的顺序如何衔接

F6 已经提供公共 ABI 回归锁（红/绿/相机）。这正是第一次建议「先 F6 再整理」所要的刹车。更新后的顺序：

1. **F6 差量内可单独翻盘的修复（6.1–6.5）**：分层、`sizeof`、validate/版本辅助、SceneRoot 进入既有 raster 测试、文档/样例。不拆 4000 行 `.mm`。做完后，F6 作为里程碑可以从「结构不批准」改为「差量批准、整树仍不批准」。
2. **第一次 3.8 的 1–3**：单一 `pack_task_record`、提交主意图标签、共享 stage 骨架。现在有 F6 的 submit 回归，这些重构的可观察合同比第一次时更完整。
3. **切开 `metal_device_hal.mm` 与 3337 行 vertical slice**（第一次 3.2 + 本次 6.6）。
4. **F8 窗口/呈现**。窗口化是新的提交形状（可呈现的 attachment + 帧节奏），不是 `ExecutionPlan` 上第 12 个 `optional`，也不是 `.mm` 里又一段 if。

在第 1 步完成之前，不把 F6 差量视为已关闭的结构工作。在第 2–3 步完成之前，不把整树视为 F8 的健康基线。

### 6.8 明确仍不做的事

与第一次相同：不重写公共 C ABI 函数表、不把 Metal 与 Vulkan 合成一套 GPU API、不删除 Reference oracle、不为了行数拆 `vg.h`。

F6 差量上额外不做：不把混合 compute+raster 顺手做进这次修复（那是 ADR-047 的重开，需要独立合同）；不把 capture relocation 假装成已实现（保持明确拒绝，直到有消费 `VG_SCHEMA_SCENEROOTRASTER_RELOCATION_*` 的 capture 版本）。

### 6.9 裁决

**(a) F6 差量作为里程碑：不批准。** 合同形状可以保留。实现过不了门槛：core→`vg.h` 分层泄漏、布局多份表达加手算尺寸、validate 复制循环、无新指针却扩张版本 OR 矩阵、每 draw identity 分配、双后端变换不对齐且无共同 oracle、文档与样例仍描述虚构或不对应的 API。把 6.1–6.5 做完后，**(a) 可以单独翻盘**，不必等待 Metal 主文件拆分。

**(b) 整树作为 F8 的健康基线：不批准。** 与 (a) 独立。第一次列出的 `ExecutionPlan` 积类型、Metal 四合一、Vulkan 注释映射、`pack_task_record` 双写、API 层解析、生指针句柄全部仍在。整仓新看到的 3337 行 vertical slice 与文档分叉，使窗口化更可能落在错误的插入点上。

---

## 附录 B：第二次审查与 rubric 的对照

| Rubric 优先级 | 落在本文 |
|---|---|
| 结构债 / 结构回归 | 5.2（core→vg.h）、5.3（布局多写）、5.6（测试 monolith）、4.10（第一次债仍在） |
| 可见却未做的大幅度简化 | 6.2–6.3（sizeof、validate/版本谓词、常驻 identity buffer） |
| 控制流上的特判增长 | 4.5–4.6、5.5 |
| 边界与类型契约 | 5.2、5.4 dummy IR、5.6 文档分叉、capture 拒绝（诚实，保留） |
| 文件体量 | 4.3 F6 新文件过关；4.9 测试树不过关；`.mm` 4414 |
| 模块性 | 5.4 测试轨道、5.6 CMake 样板复制 |
| 可读性 | 5.4 冒烟当规格、PPM 工件 |

第一次附录表仍然描述 `b6bd6db` 快照。本表描述 F6 之后的工作树。
