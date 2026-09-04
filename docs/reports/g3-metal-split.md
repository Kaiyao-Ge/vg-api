# G3 Metal 职责拆分交付记录

日期：2026-09-03。Owner：`g3_metal_split`。依据整改报告 §9.3 / §10.1、
START、原设计 Stage 0—7、ADR-053/054，以及
[G3/G4 执行冻结](g3-g4-orchestration.md)。

当前状态：实施、本机验证、真实 Metal 平台验证、本文记录完成；等待统筹独立审查。
这不是 G4、G5/G6 或整个 §9.3 完成声明。Linux SDK/真实 Vulkan 门禁没有在 G3 关闭。
未切换分支、提交或推送；保留 `main` 上全部既有 G0/G1/G2 成果。

## 1. 冻结范围与实际改动

起点为 `99ec414` 上已经复核的 G0/G1/G2 工作树；完整前序检查点由统筹保存于
`build/g12-checkpoint/g12.patch`。G3 自己的原 Metal 源、头、测试副本及机械搬迁核查
保存在忽略目录 `build/g3-baseline/`，不将这些本地产物伪装成远程备份。

本包只移动既有行为，不改变公共 `hal::DeviceHal` 虚接口、公共 C ABI、schema、
`VgTaskRecordV2`、capability、Stage 顺序、NodeRef、Task shape、Mixed-domain 限制、
continuation、poison、hold、representation 或 completion 合同。

实际生产文件均位于 `src/backends/metal/`：

| 文件 | 职责 / 原符号迁移 |
|---|---|
| `metal_device_hal.h/.mm` | 薄 Device 门面；创建/指定 UUID 创建、能力快照、buffer probe、Node-aware 只读观察；`compile/submit` 委托私有 `compile_plan/submit_plan` |
| `metal_device_internal.h` | 唯一 `DeviceHal::Impl` 状态、私有方法声明、Allocation/Facet record、FacetUseGuard 声明；没有把原巨型实现搬成内联头 |
| `metal_physical_types.h` | 生产 raster 与窄 harness 共用的 AttachmentDesc / RasterDesc / RasterVertex / RasterResult 值类型；不含 Metal 对象 |
| `metal_resources.mm` | Allocation buffer、Facet texture/backing、上传/读回、identity-root cache、回收、物理 view 转换/检查、FacetUseGuard |
| `metal_pipelines.mm` | library / per-Node pipeline / sampler / depth-stencil cache、既有物理 probe pipeline；key、compile/cache-hit 行为不变 |
| `metal_lowering.mm` | `CompileOps`、`compile_plan`：Stage 6 preflight、per-Node packages、representation physical operation、timeline 与 sealed schedule lowering |
| `metal_commit.mm` | `SubmitOps`、`submit_plan`：唯一 Stage 7 顺序、真实 schedule 执行、publication、failure/poison、completion；保留 plan-driven raster 的绑定编排 |
| `metal_encoding.mm` | `ensure_timeline_event`、`dispatch_and_wait`、`dispatch_indexed_and_wait`、`dispatch_compute_task`、`dispatch_task_publish`、`dispatch_task_tier1_indirect` 物理命令实现 |
| `metal_raster.mm` | `make_render_pass`、`run_raster_pass`，由生产 submit 与 test harness 共用的真实 draw/readback |
| `metal_representation.mm` | `transform_into_private_facet`：既有 physical blit 与 backend backing 安装，不推导 Core 语义 |
| `metal_diagnostics.h/.mm` | `DispatchStats` 累加、`make_facet_report`、`apply_dispatch_stats`；只归集真实编码/完成位置产生的数值 |
| `metal_shader_sources.h/.mm` | 原内嵌 storage shader 的唯一文本定义，字面量不变；pipeline 只取 source 编译 |

`metal_physical_types.h`、`metal_representation.mm`、`metal_shader_sources.h/.mm`
在实施前向统筹报告了必要性，已获确认并加入冻结清单。`metal_probe.mm` 和
`metal_tier2.*` 未改动；Tier2 继续使用 G0 的测试支撑库。

测试与登记文件：

- 新增 `tests/support/metal_adapter_harness.h/.mm`：非 owning 的 `AdapterHarness`
  引用现有 Device；迁入 cull、Address/Sample/Storage/Attachment facet、standalone
  representation transform、raster triangles、pipeline classification、Tier1/indexed
  实验入口及 Tier1 观察器。设备必须比 harness 活得更久；观察器返回的是设备保存的数据，
  不是临时 harness 的存储。
- `tests/vertical_slice/metal_task_timeline_test.cpp`：仅新增 include，将原 `device->run_*`
  / 回收 / Tier1 观察调用改成显式 `AdapterHarness(*device)` 接收者。没有拆测试函数、
  改断言、改 oracle、改 case 或用 harness 替代 plan-driven compile/submit。
- `tests/vertical_slice/metal_identity_scene_root_cache_test.cpp`：仅新增
  `metal_physical_types.h` include，继续使用原 `RasterVertex` 定义。
- `tests/tools/test_schema_generator.py`：identity buffer 状态检查读 private header，
  创建/复用检查读 resources，实际 report 检查读 commit，Tier1 schema-word 检查读 encoding。
  禁止本地 codec 的原断言覆盖 Metal `.mm` 与迁出的 harness，未删断言或插入假标记。
- `cmake/g3-metal-sources.cmake`：显式登记九个新增生产 `.mm`；不重复登记 facade。
- `cmake/g3-metal-tests.cmake`：创建 `vg_metal_adapter_harness` 测试支撑库，仅链接既有
  `vg_metal_task_timeline_test`；不修改 CTest 名称、命令、属性或 skip。

根 CMake、Core、Compiler、Reference、Vulkan、API、ABI、schema、其他测试用例与
总整改台账没有由 G3 修改。本文是本包唯一新增报告。

## 2. 符号清单与依赖/所有权核对

关键方法的唯一 owner：

- **Resources**：`release_buffer`、`release_facet_textures`、`retire_stale_facet_textures`、
  `release_empty_linear_buffers`、`reclaim_released_backing`、`ensure_buffer`、
  `make_identity_scene_root_buffer`、`commit_buffer_write`、`facet_cache_key`、`resolve_facet`、
  `ensure_facet_buffer`、`read_texture_region`、`read_texel`、`install_facet_record`、
  `upload_view_subresources`、`ensure_facet_texture`、`probe_gpu_addresses`、
  `ensure_guard_placeholder_texture`，以及原 `Impl` 析构、FacetUseGuard 和 format/view helpers。
- **Pipelines**：`ensure_pipeline`、`ensure_node_pipeline`、`ensure_sampler_state`、
  `ensure_task_ring_pipeline`、`ensure_cull_compact_pipeline`、`target_identity`、
  `make_pipeline_key`、`ensure_library`、`ensure_function`、`acquire_compute_pipeline`、
  `acquire_render_pipeline`、`ensure_depth_stencil_state`、`ensure_sample_facet_pipeline`、
  `ensure_raster_pipeline`、`ensure_storage_facet_pipelines`。
- **Lowering**：原 `CompileOps::{init,fail,reject_unsupported,select_packages,
  representation_requests,timeline,execution_schedule,pipelines}` 与 `compile` 函数体；
  原 `is_pointer_graph_module` 保持一个定义，供 lowering / commit 消费，
  `plan_computes_over_allocation` 留在 lowering。
- **Commit**：原 `SubmitOps::{take,generations,bind,begin,stage5,raster,precheck_timeline,
  sealed_effects,execute_schedule,publish_tasks,publish}`、`publish_envelope_order` 与
  `submit` 函数体。`apply_stats` 仅改名并移动到 diagnostics 的 `apply_dispatch_stats`。
- **Harness**：`run_cull_compact`、`run_address_facet`、两个 `run_sample_facet` 重载、
  两个 `run_storage_facet` 重载、`run_attachment_facet`、`run_representation_transform`、
  `run_raster_triangles`、`run_pipeline_classification`、`run_task_tier1_indirect_test_harness`、
  `run_indexed_compute_test_harness`、standalone `reclaim_released_backing`、
  `last_tier1_indirect_dims`。

依赖方向保持为：

```text
DeviceHal 门面 → Stage 6 lowering / Stage 7 commit
                       ↓
             pipelines + resources + physical encoding/raster/representation
                       ↓
             真实统计 → diagnostics 汇总

tests → AdapterHarness → 相同 backend 私有物理操作
生产库不依赖 tests / AdapterHarness 实现
```

所有 cache/MTL 对象仍存于原 Device 的同一个 `Impl`，没有拆成多个缓存副本或改变
失效键。原 `retain/release`、析构顺序、错误清理和同步等待的函数体保持；本包没有
借机改变既有资源回收策略。实验的 singleton pipeline helper 仍是 backend 私有物理
机制，生产 Node-aware 路径继续只用 per-Node pipeline；它们不成为第二条 submit 入口。

Stage 7 仍按原顺序执行：compiled-plan 校验 → begin → Timeline 预检 → sealed effects →
一次 continuation admission → hold prepare → 已编译 representation physical operation →
hold acquire → 完整 canonical publication → sealed schedule → completion 后释放 hold。
没有把 `task_order` 或 ring bytes 重新当作另一份执行事实。

## 3. 守恒与静态门禁

机械搬迁工具只在忽略目录使用，修改通过 `apply_patch` 应用；没有注册新的递归 CTest。
最终源码检查结果：

- 109 个原函数体（包括重载、构造/析构、统计运算符）都有唯一新 owner。比较忽略空白和
  注释但保留字面量；允许变化只有私有委托名称、stats helper 名称、harness 接收者，以及
  原 storage source 声明替换为读取其新唯一 owner 的函数调用。
- 原 header 的 21 个数据/枚举定义逐字相同且各有一个 owner；`DeviceHal` 类本身按目标收窄。
- storage shader 字面量逐项完全相同；未更改 shader、schema offset 或 ring layout。
- `metal_task_timeline_test.cpp` 撤销 include/receiver 适配后，与修改前全文完全相同。
- 四个纯 C++ 表面头和 Objective-C++ private header 自包含编译通过。
- `nm -C build/g3-metal/libvg_backend_metal.a` 无 `AdapterHarness`；`ar -t` 只含十个生产
  Metal 编译单元，不含 harness/Tier2 对象。测试支撑库仅由原实验测试目标消费。
- 以下检查零命中：生产反向 `#include ... tests/`、生产 `AdapterHarness(...)` 调用、
  facade `DeviceHal::run_*`/facet 实验入口、旧 `request_*`/effect-dag 请求袋、
  first-Node projection、backend `topological_order()` 重建。

```sh
rg -n '#include.*tests/|AdapterHarness\(' src/backends/metal
rg -n 'DeviceHal::run_|bool run_.*facet|bool run_raster_triangles|bool run_cull_compact' \
  src/backends/metal/metal_device_hal.h src/backends/metal/metal_device_hal.mm
rg -n 'resolved_nodes(\.front\(\)|\[[[:space:]]*0\])|per_node_packages(\.front\(\)|\[[[:space:]]*0\])|topological_order\(' \
  src/backends/metal --glob '*.mm'
rg -n 'request_tier1_indirect|request_tier2_select|request_indexed_binding|effect_dag_passes|effect_dag_dependencies' \
  src/backends/metal --glob '*.mm'
git diff --check
```

知识图在搬迁前后均重新索引；工具未完整提取 Objective-C++ 函数体，因此调用与 owner
以源码补证，不把图中的缺边当作无依赖证明。

## 4. 构建与回归证据

使用独立 `build/g3-*` 目录；测试子进程均独占 `TMPDIR`。所有构建为 Debug + ASan/UBSan。
真实 Metal CTest 和 OFF smoke 在非沙箱执行，probe 确认 `Apple M1` /
`Version 26.6.2 (Build 25G83)`，不是 Reference 回退。

```sh
cmake -S . -B build/g3-reference -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DVG_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/g3-reference -j4
g3_test_tmp=$(mktemp -d /private/tmp/vg-g3-reference.XXXXXX)
TMPDIR="$g3_test_tmp" ctest --test-dir build/g3-reference --output-on-failure

cmake -S . -B build/g3-metal -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DVG_ENABLE_METAL=ON -DVG_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/g3-metal -j4
g3_test_tmp=$(mktemp -d /private/tmp/vg-g3-metal-final.XXXXXX)
TMPDIR="$g3_test_tmp" ctest --test-dir build/g3-metal --output-on-failure
g3_test_tmp=$(mktemp -d /private/tmp/vg-g3-metal-repeat.XXXXXX)
TMPDIR="$g3_test_tmp" ctest --test-dir build/g3-metal \
  -R '^(api.multicode-taskgraph-conformance|api.mixed-domain.(reference|metal)|core.execution-plan|conformance.device-hal.metal|vertical-slice.metal|vertical-slice.metal.(identity-scene-root-cache|mixed-domain|effect-dag|consume-input|representation-churn|checked-facet-generation))$' \
  --repeat until-fail:20 --output-on-failure

cmake -S . -B build/g3-off-reference -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF \
  -DVG_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/g3-off-reference -j4
cmake -S . -B build/g3-off-metal -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF \
  -DVG_ENABLE_METAL=ON -DVG_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/g3-off-metal -j4
python3 tests/tools/check_build_boundary.py build/g3-reference build/g3-off-reference
python3 tests/tools/check_build_boundary.py build/g3-metal build/g3-off-metal
build/g3-off-reference/vg-platform-probe --validate
build/g3-off-reference/vg-offscreen-triangle-ppm build/g3-off-reference/g3-smoke.ppm
build/g3-off-metal/vg-platform-probe --validate
build/g3-off-metal/vg-offscreen-triangle-ppm build/g3-off-metal/g3-smoke.ppm

python3 tests/tools/check_core_headers.py --generated-dir build/g3-reference/generated --baseline-ref 99ec414
python3 tests/tools/check_compiler_sources.py build/g3-reference --compare build/g2-reference/before.bin
python3 tests/vertical_slice/vulkan_capability_contract_test.py .
```

| Gate | 实际结果 |
|---|---|
| Reference 全量 | 38/38，16.71 秒，无 skip |
| 真实 Metal 全量 | 首轮 72/72，44.29 秒；最终 include 收口后再次 72/72，30.55 秒，无 skip |
| Node-aware / mixed / lifetime / ConsumeInput / cache / checked-facet 重复 | 12 项各 20 次，240/240，39.91 秒，无 skip |
| Reference / Metal OFF 全目标编译链接 | 均通过；生产编译单元分别 41 / 52，OFF CTest 均为 0 |
| G0 ON/OFF 边界 | 两 profile 均通过；测试 name/command/properties 映射摘要与 G0 完全相同 |
| OFF 公共 ABI smoke | Reference 与真实 Metal 均输出 128×128 PPM，只写入各自 build 目录 |
| G1 显式门禁 | 64 个原类型、1409 行实现守恒；16 个自包含头、无 include 环或逆向依赖 |
| G2 显式门禁 | 24567 字节完全一致；10 source functions、17 inputs × 3 builders |
| Vulkan source-contract | 本轮通过；不代表 Linux SDK 或 GPU 验证 |
| 文档/静态 | 全量中的 docs.check 通过；git diff --check 通过 |

CTest 映射 SHA-256：Reference `0ccb72bbf83edbd11ee2ebe08c47926231cc71d6300bde312160f7c80a0ce63f`；
Metal `2651d34acbf47db9be9940d3cf690f3f3fc24f995bc5f737f5335e0e34282ace`。
Compiler 输出 SHA-256 仍为 `dee036f482a910f9af0698817a241689c45a1d26f8321585796c4b7a659cffbd`。

搬迁的中间构建曾发现 helper 声明生成的多余控制行，以及跨 TU 的
`is_pointer_graph_module` 声明缺失；均在包内集中修正，然后重新构建、全量验证。
没有修改语义或放宽测试来取得通过。

## 5. 剩余边界

- G3 等待统筹独立审查，未自行宣布 clean integration 或开始 G5。
- `metal_task_timeline_test.cpp` 与 harness 仍可在 G5 按测试职责分轨；本包不以行数为由
  提前拆测试函数。现有 capture.view 临时路径风险仍按 G1/G2 记录，通过独占 TMPDIR 隔离。
- Vulkan 在并行 G4 中处理；本报告的 source-contract 结果只对应本轮执行时点，后续
  跨包集成必须重跑共享检查。Linux SDK/真实设备验证没有被本地 Metal 结果替代。
- 没有修复未授权的并发 destroy/submit、资源回收新政策、性能优化或未来执行域。
  增加 ray/neural 时仍扩展既有 contract/schema/capability/lowering，无新增平行 submit。
