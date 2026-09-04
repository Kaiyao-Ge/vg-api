# G4：Vulkan 拆分前置核查与迁移清单

日期：2026-09-03。Owner：`g4_vulkan_split`。
当前状态：**源码拆分已实施；本机37/38，待统筹适配共享schema路径；远程Linux SDK/GPU验证pending**。

用户已找到远程测试机器，明确要求先按本报告规划拆分，不再以平台验证阻塞开工。
统筹已修订[G3/G4执行冻结](g3-g4-orchestration.md)：允许冻结范围内的源码、测试路径和
CMake fragment改动；远程验证仍是最终验收门禁。下面内容保留首次准备阶段的事实快照，
其中“awaiting-platform/解锁前不得搬迁”描述的是旧开工顺序，不再阻止当前实施。

以下 §1—8 是首次准备阶段的记录；当前源码实施与本机结果见 §9，不以历史未开工状态覆盖当前进度。
首次交付的是已核查的拆分准备，不是源码拆分完成记录。依据
[G3/G4 执行冻结](g3-g4-orchestration.md)、[G0](g0-build-baseline.md)、
[整改报告 §9.3/§10.1](thermo-nuclear-code-quality-review-main.md)；保留
main `99ec414` 上 G0/G1/G2 和并行 G3 的全部修改。本轮仅新增本报告，未修改
Vulkan 生产文件、测试、CMake，未切换分支、提交或推送。

## 1. 平台门禁结论

G0 冻结了先取得 Linux SDK 构建及真实 Vulkan 设备基线，再搬生产文件的顺序。
本轮只读核查结果：

| 核查 | 实际证据 | 能否解锁 |
|---|---|---|
| `uname -s` | `Darwin` | 否 |
| `command -v glslc vulkaninfo docker` | 仅 `/usr/local/bin/docker` | 否 |
| `docker context ls` | `default` 与 `desktop-linux`，均指向本机 Unix socket | 否 |
| socket 文件 | `/Users/gokyrie/.docker/run/docker.sock` 不存在；`/var/run/docker.sock` 是指向它的符号链接 | 否 |
| 仓库 `.github/workflows` | 只有 `codeql.yml`；Ubuntu 作业显式 `VG_ENABLE_VULKAN=OFF` | 否 |
| `CMakePresets.json` | `dev-vulkan`/`perf-vulkan` 限定 Linux，提供构建配置而非远程执行环境 | 否 |
| 项目复现文档/配置 | 提供本地 Linux 命令；未发现明确可用的 GPU runner 或 SSH 执行目标 | 否 |

没有启动 Docker、安装软件、拉取镜像、尝试猜测的 SSH 目标、扫描凭据或触发远程 CI。
即使容器只提供 SDK，也不能冒充真实 GPU 基线；macOS stub、syntax-only 和静态合同均不能替代它。
需要用户/统筹提供已授权的 Linux checkout/执行入口，SDK+loader+ICD+glslc，以及可访问的真实
Vulkan GPU；先取得第 7 节基线，再由统筹明确解锁源码搬迁。

## 2. 已核查的代码事实

已读 START、整改规则、G0/G1/G2/G3/G4 冻结、整改报告 §7—10、原设计
02/03/05/07/08/09/10、ADR-053/054。ADR-053 中旧 backend narrowing 不能覆盖 START 和
ADR-054 当前实施状态；Vulkan concrete Raster 整计划拒绝保持不变。

使用 codebase-memory 技能重新索引当前工作树为 `vg-api-g4-vulkan-plan`（fast、
`persistence=false`），查询 schema、Vulkan architecture、Function/Method/Class owner，
随后以源码与 `rg` 补证。索引排除了 docs/tools/tests/tools，且遗漏部分嵌套类型和内联方法，
所以不能把图中零 caller 当作删除依据。以下行号是本轮未拆分 Vulkan 源码的定位快照。

当前主文件 `src/backends/vulkan/vulkan_device_hal.cpp` 为 4,149 行。

- Stage 6 `compile`（2054）拒绝具体 Raster Node、discovery、working-set，以及线性 BDA
  package 不支持的 pointer graph；成功时按完整 NodeRef 记录 package。
- `compute_pipeline_cache_key`（41）是完整 package/entry 内容键，不是简单 NodeRef 数值键。
  完整 NodeRef 选择 package，device-owned cache 允许相同 immutable package 内容复用；
  拆分不改变这一事实，不为迎合名称而重新设计 cache key。
- Stage 7 `submit`（2253）检查 CompiledPlan backend/版本、transition、逐 Node package，
  消费 Core sealed components/waves；`dispatch_task_graph`（788）绑定每个 Task 的 pipeline
  和 addresses，并使用自己的 x/y/z。没有理由在本包重做 Node-aware 或图算法。
- Timeline precheck → 一次 continuation → lifetime prepare → representation commit →
  lifetime acquire → task dispatch/completion → writeback → ring publication 的顺序有现有保护。
  continuation 只过滤 publication，不能偷偷改成过滤程序执行。
- ring pipeline/三个临时 buffer 与 canonical compute pipeline 分开；ring 不是执行 authority。
- `ensure_facet_image`、`transform_representation`、stale image retirement 是生产 representation
  路径需要的物理实现，不能因为名称含 facet 就全部搬到 tests。
- **目前没有 Vulkan 四个纯物理入口的实际外部调用测试。** 全仓 src/tests 搜索仅找到定义、
  声明和注释；`vulkan_task_timeline_test.cpp` 的四种模式均是 assembler-driven。
  不得宣称已有 Vulkan sample/storage/raster/classification GPU 回归。
- Raster capability 当前未广告；`run_raster_facet` 的非空 draw 与
  `run_pipeline_classification` 也以此拒绝。G4 不绕过这个拒绝来制造正向证据。

## 3. 文件与类型 owner 迁移方案（尚未实施）

以下目标均为拟新增路径，不代表文件已经存在。后端目录前缀为
`src/backends/vulkan/`。不引入公共接口、第二套提交对象或资源生命周期。

| 目标 | 唯一职责及类型 |
|---|---|
| `vulkan_device_hal.h/.cpp` | 薄门面：DeviceHal 创建/析构入口、capabilities、compile/submit 委托、两个 make_device_hal；不再公开 run_* 实验入口或 Vk* 存储 |
| `vulkan_device_internal.h` | device-owned 私有状态及内部函数声明；FormatSupport、AllocationRecord、ComputePipelineRecord、TaskRingBuffers、VulkanFacetRecord、FacetImageKey、CanonicalTaskDispatch、TaskDispatchCounts、RepresentationStageCounts、RawBuffer；保留比较规则，非大段内联实现仓库 |
| `vulkan_resources.cpp` | buffer/image/view/memory/sampler/descriptor pool 的创建、上传、retirement；保留 transform 的物理实现与 backing-release owner |
| `vulkan_pipelines.cpp` | GLSL→SPIR-V 子进程、shader/layout/pipeline cache；所有缓存仍 device-scoped |
| `vulkan_lowering.cpp` | Stage 6、representation 可表达性、wave transition physical lowering |
| `vulkan_commit.cpp` | Stage 7 唯一顺序、完整 NodeRef→package 查找、hold/continuation、结果与 publication 组织；不搬成第二个语义 runtime |
| `vulkan_encoding.cpp` | command pool/buffer/fence/timeline 物理操作、canonical dispatch、ring publication、image barrier |
| `vulkan_raster.cpp` | 已有 attachment/raster 物理 pass；不因此开放生产 Raster |
| `vulkan_diagnostics.h/.cpp` | set_error、报告初始化/归集；计数产生点仍在真实 Vk 调用处 |
| `tests/support/vulkan_adapter_harness.h/.cpp` | 四个纯物理实验入口及 sample/storage/classification 测试编排，复用 backend 私有接口 |
| `cmake/g4-vulkan-sources.cmake` | 新生产 .cpp 显式登记一次；原 facade 根 CMake 已登记，不重复 |
| `cmake/g4-vulkan-tests.cmake` | 仅测试启用时编译/链接窄 harness；不得向生产 archive 注入 tests 实现 |

类型分界：

- `FacetDescriptorCost`、`AttachmentLoadAction`、`AttachmentStoreAction`、`RasterPassDesc`、
  `RasterPassResult` 若被 backend 物理 pass 消费，放 backend-private 声明，harness 只使用它们；
  不能让生产文件 include tests，也不复制同义类型再手工转换。
- `StorageFacetTarget`、`SampleFacetResult`、`StorageFacetResult`、`RasterPipelineVariant`、
  `PipelineClassificationResult` 由纯实验接口使用，移入 harness 声明。
- `FacetUseGuard` 是 transform 和物理 harness 共用的既有 RAII bracket；声明归私有头，
  begin/析构定义归 resources，不能给测试另造一份生命周期规则。
- 当前四个内联 `instance()/physical_device()/device()/compute_queue_family()` 在 src/tests
  未找到调用。收起 SDK 成员时不应继续在门面暴露原生句柄；若实现时发现真实 caller，先报告
  并分类，不新增公共虚接口。`VG_HAS_VULKAN` 不能让使用者看到与库不同的门面对象布局。

### 3.1 逐函数迁移表

自由函数和成员函数全部按现有名称列出；多函数一行表示同一 owner，不表示合并它们。

| 原符号（位于 `vulkan_device_hal.cpp`，除注明外） | 拟 owner |
|---|---|
| `set_error` (29)、`make_facet_report` (713) | diagnostics |
| `append_cache_key_component` (34)、`compute_pipeline_cache_key` (41) | pipelines；compile/submit 共用一个声明与实现 |
| `same_compute_bindings` (60)、`same_vulkan_compute_package` (68)、`node_ref_equal` (76) | commit 的 package 完整性检查；不扩大到 Core |
| `lower_wave_transitions` (85) | lowering |
| `find_memory_type` (106)、`destroy_raw_buffer` (252)、`create_raw_buffer` (259) | resources |
| `compile_glsl_stage` (131)、`compile_glsl_to_spirv` (238)、`ensure_pipeline` (318) | pipelines |
| `ensure_buffer` (390) | resources |
| `ensure_command_pool` (461)、`allocate_command_buffer` (473)、`submit_and_wait` (488)、`submit_and_wait_simple` (525) | encoding |
| `to_vk_format` (541)、`to_vk_swizzle` (549)、`to_vk_component_mapping` (564)、`packed_swizzle` (573)、`to_vk_view_type` (578)、`facet_read_layout` (586)、`facet_image_usage` (601) | resources；必要者私有声明供物理代码共用 |
| `storage_image_format_qualifier` (545)、`storage_facet_glsl_source` (742) | pipelines 的私有 storage shader 源；字节保持不变，不挪入 package orchestration |
| `layout_sync_scope` (615)、`record_image_barrier` (652)、`record_layout_transition` (1093) | encoding；仍维护同一 VulkanFacetRecord 的 layout |
| `FacetUseGuard` 构造/delete-copy/delete-move、`begin`、析构 (691 起) | 私有声明/resources 定义 |
| `decode_first_texel` (720)、`encode_first_texel` (1064) | 私有物理 texel helper；resources 定义供 raster/harness 共用，不能放 tests 再被生产调用 |
| `to_vk_sample_count` (754)、`to_vk_load_op` (763)、`to_vk_store_op` (777) | raster；sample-count 映射供 pipeline 私有调用 |
| `dispatch_task_graph` (788)、`ensure_timeline_semaphore` (873)、`dispatch_task_ring_publication` (1008) | encoding |
| `ensure_task_ring_pipeline` (887) | pipelines |
| `create_task_ring_buffers` (955)、`destroy_task_ring_buffers` (998) | resources；仍使用 schema word 常量 |
| `resolve_facet` (1077)、`ensure_facet_image` (1102)、`retire_stale_facet_images` (1348)、`ensure_sampler` (1369)、`ensure_descriptor_pool` (1407) | resources |
| `ensure_sample_facet_pipeline` (1429)、`ensure_storage_facet_pipeline` (1532)、`ensure_raster_shader_modules` (1597)、`ensure_raster_pipeline` (1654) | pipelines |
| `transform_representation` (1811) | resources 的既有物理 transform；Stage 7 调用点仍归 commit |
| `~DeviceHal` (1881) | facade 入口，唯一私有 owner cleanup；可调用 resources/pipelines cleanup，但不得改变顺序或重复析构 |
| `capabilities` (1954)、`create_impl` (3850)、两个 `make_device_hal` (4141/4145) | facade；UUID 选择、feature negotiation、失败清理保持 |
| `can_lower_representation_requests` (1981)、`compile` (2054) | lowering；facade 仅委托 |
| `submit` (2253) | commit；facade 仅委托 |
| `run_sample_facet` (2779)、`run_storage_facet` (3098) | harness，保持原检查、guard、descriptor/command/oracle 事实 |
| `run_raster_facet` (3317) | harness 入口；原物理 pass 实现归 raster，以 private 调用复用，不接入 production Raster submit |
| `run_pipeline_classification` (3725) | harness；两组 pipeline cache 物理创建/销毁仍由 pipelines owner 完成 |
| 头内 `format_support`、`FacetImageKey::operator<`、DeviceHal 构造/delete-copy/delete-move | 私有状态声明/对应 owner；完整 tuple 和禁止拷贝语义不改 |

函数局部类型 `PendingBuffer`（sample descriptor 写入的临时数据）跟随 sample harness；
publication push-constant 匿名结构跟随 encoding。lambda 的清理逻辑跟随其拥有的函数，不能
把 capture 的引用跨出其对象寿命。不要仅为凑目标文件数量抽取没有独立责任的碎片。

### 3.2 实际依赖方向

```text
API → Vulkan facade → lowering / commit
lowering → compiler packages + pipelines + Core sealed facts
commit → resources + encoding + immutable Node packages + shared HAL helpers
resources → encoding（上传/物理 transform 要等待完成）
encoding → 私有 record 声明 + Vulkan API（不反调高层 resource planner）
pipelines → compiler shader sources + Vulkan API
raster → resources + pipelines + encoding
test harness → backend 私有 physical 接口
production ↛ tests；Core ↛ Vulkan SDK
```

一个 private state 仍由一个 DeviceHal 唯一拥有；职责文件只借用引用，不能各创建一个 device、
facet cache、command pool 或 continuation store。`std::map` pipeline record 的地址在后续
compile 时保持稳定，不替换为会使已准备 dispatch 指针失效的临时容器。

## 4. 资源寿命与失败清理冻结

| 资源/边界 | 当前必须保留的行为 | 审查方式 |
|---|---|---|
| instance/device/queue | UUID 精确选择；feature 查询与启用保持一致；create_impl 中途失败由唯一 owner 清理已创建句柄 | 对照每个 early return 与析构 |
| allocation mirror | generation/容量不符才淘汰；先 unmap，再 destroy buffer、free memory；host-coherent 假设不改 | generation、重复 submit、输出 bytes 对照 |
| compute pipeline cache | shader/layout/pipeline 保留到 device 销毁；compile B 不使已编译 A 失效 | 多 Node 与 A/B/A 重用测试 |
| glslc subprocess | pipe/file actions/argv 字符串/pid 的作用域及 stderr 诊断不改 | SDK 真编译；逐失败分支核对，不在本包重写 subprocess 算法 |
| command buffer/fence | 分配后 begin/end/submit 失败释放；queue submit 后 fence wait 才释放可被 GPU 引用的临时资源 | Vulkan validation 与源码对照；不新增异步承诺 |
| timeline | semaphore 是真实 counter authority；precheck 在 continuation/representation 之前；zero wait/signal 省略该侧 | timeline/continuation oracle 与实际 counter |
| ring buffers | state/fields/inputs 每 submit 创建；部分分配失败完整回滚；pack/dispatch/unpack 失败都清理 | task-tier0、配额/恢复/重复测试 |
| facet image cache | 完整 index/generation/epoch/kind/format/view/extent/layer/mip/swizzle；cache hit 先于读取已 Consume 的 linear bytes | representation/consume 与真实物理补证 |
| image/layout | 只由 record_layout_transition 更新；layout barrier 与 representation transform 分开报告 | 原 barrier 类型/数量/顺序逐项对账 |
| image creation/upload | image/view/memory 与 staging 失败清理；subresource copy offset/row/layer/mip 来自 CanonicalView | 多 mip/array 与异常资源路径 |
| descriptor pool | 只有前次同步完成后才能 reset；sample array/checked specialization 保留不同真实 bindings | shader/schema 与 descriptor count 对照 |
| temporary facet use | FacetUseGuard 覆盖 GPU 引用期；transform 的 TransferFacet guard 结束后 retire | stale/active-use/early return 不漏 hold |
| representation commit | Core 决定 epoch/consume；HAL 只执行 physical operation；release old buffer 在真实 transform 完成后 | prepare→commit→acquire 顺序不变 |
| plan-local hold | 每 submit 独立 RAII；发生 early return 也释放；Node/CodeObject immutable snapshot 不写回 graph | lifetime gate 与 sanitizer |
| raster transient image | MSAA image/view/memory、readback、descriptor payload 在 fence 完成后销毁；两个 guard 各自配对 | 仅在已授权的物理验证范围内运行 |
| specialization measurement | naive/classified cache 不能互相污染；替换同键 pipeline 前销毁旧对象 | 现阶段 Raster 未广告仍应拒绝，不绕过 guard |
| result/report | 编译历史成本保留，planned 执行事件清掉；真实编码处产生 dispatch/barrier/wait 计数 | success、early reject、device failure 路径分别比对 |

这是拆分必须复核的清单，不是声称所有旧失败路径已通过真机验证。若 Linux 基线暴露既有
P0/P1，先单列基线阻塞与有界修复，不把它隐蔽混入机械搬迁。

## 5. 测试与静态路径迁移清单

### 5.1 既有路径：保留 oracle、名称、参数和 skip 合同

| 文件 | 当前归属与 G4 处置 |
|---|---|
| `tests/vertical_slice/vulkan_task_timeline_test.cpp` | `run_task_tier0` (161)、`run_timeline` (616)、`run_raster_rejected` (722)、`run_raster_msl_rejected` (797)；真实 assembler-driven，不是 facet harness。当前没有需改的 run_*facet 调用；至多必要 include/注释路径适配，不拆 G5 测试 |
| `tests/vertical_slice/vulkan_bda_vertical_slice_test.cpp` | canonical compute/BDA 与 Reference oracle，保留门面 include 和生产 compile/submit |
| `tests/conformance/device_hal_conformance_vulkan.cpp` | 真实 device 必须创建成功；共享 conformance、publication/timeline 期望不改 |
| `tests/api/vg_mixed_domain_conformance_test.cpp` | `api.mixed-domain.vulkan` concrete Raster 整计划拒绝与无部分执行；不改 ABI 测试 |
| `tests/vertical_slice/vulkan_capability_contract_test.py` | 唯一必须随新 owner 迁移的 Vulkan 静态 source-contract 文件，详下表 |
| `tests/tools/test_schema_generator.py` | 共享 owner 为 G3/统筹，G4不并发编辑；`kTaskRingDispatchXWord` 检查应读 resources 的 ring buffer owner；禁止本地 codec 检查覆盖 facade/lowering/commit/encoding/resources/raster 等生产 owner |
| `src/api/vg_api_device.cpp` | 门面 caller；PIMPL 后不应需要 SDK 类型，不改 API dispatch |
| `src/backends/vulkan/vulkan_probe.cpp` | 独立 probe；只允许必要 include 适配，不改变 capability |
| 历史 ADR/report 的旧文件定位 | 历史证据不批量重写；当前报告给迁移映射，真实 markdown 断链需修复 |

source-contract 必须按实际函数 owner 分别读取，不把全部新文件拼接成一份“假旧文件”，也不靠
注释加入期望字符串过门禁：

| 原断言组 | 新检查位置 |
|---|---|
| discovery/working-set/Raster 在 preflight 前拒绝 | lowering 的真实 compile 实现 |
| EffectDag 广告，IndirectTier1/Raster 未广告 | facade/create_impl 的真实 capability 写入 |
| 每 Node package 与 pipeline compile 顺序、pointer-graph 拒绝 | lowering |
| pipeline cache 存在、无 singleton/first-node/多 Node 固定拒绝 | 私有 state + 所有生产 owner，各自检查 |
| sealed wave transition lowering、真实 dispatch shape/sync2 | lowering 与 encoding，分别定位各自函数体 |
| publication 只用 ring pipeline、无 indirect/program dispatch | encoding 的 publication 函数体 |
| continuation 恰好一次且先于 prepare/representation/acquire/dispatch/writeback/ring | commit 唯一编排函数；若委托命名变化，检查真正调用顺序而非旧字符串 |
| canonical publication/实际 transition 计数重置 | commit |
| 不重建 effect/order/components | 每个生产文件独立禁止旧符号；不将合法 Core 调用链误判为 backend 自行推导 |

### 5.2 明确的覆盖缺口

目前纯 Vulkan sample/storage/attachment/classification 入口缺少实际 caller 测试；因此迁移
harness 后“链接成功”不能等价为 GPU 行为已验收。根 CMake 当前 Vulkan driver 专属登记为
7 项：device-hal conformance、BDA vertical slice、task-tier0、timeline、两项 raster rejection、
API mixed；另有所有 profile 都跑的 Python source-contract。这不是 Linux 全套测试总数。

解锁前建议统筹另行冻结最小物理 smoke 的具体授权：sample checked/fast+stale、storage
image/linear+format拒绝、attachment-only clear/load/store（能力满足时）以及非空 raster draw/
classification 的 Unsupported。不得修改 capability 位去让 draw 通过；不得把这些新测试
追记为已存在。未经批准，本报告不添加测试、CTest 名称或新的 oracle。

## 6. 解锁后实施顺序

1. 保存 Linux 基线的源码身份、dirty diff、CTest JSON 映射、环境和真实设备结果；统筹确认解锁。
2. private state + resources/cache 的唯一 owner 先落位，所有析构路径逐项对照；生产 ON/OFF 构建。
3. pipelines/lowering 搬迁，Stage 6 输出与旧 package/report 对照，不改 glslc 源字节。
4. encoding/commit 搬迁；先保留唯一完整 submit 编排再抽私有物理函数，保护 continuation/hold 顺序。
5. raster/diagnostics 与四个窄 harness 收尾；不把生产 representation 搬进 tests。
6. source-contract/schema 路径适配、显式 CMake source/harness 登记。中央文件仍由统筹集成。
7. Linux 全量/真实 GPU/OFF/重复/静态门禁，独立审查并集中修正；再提交 G4 完成证据。

这是一个有界 G4 包的内部步骤，不另造七个待无限扩张的小包。不启动 G5 测试分轨、G6 runner，
不动 Core/Compiler/API/ABI/schema，不开放 Raster、指针图、working-set 或新 Task tier。

## 7. Linux DoD 命令（待执行，不是本轮结果）

在已授权 Linux 仓库根运行。先有 CMake 3.25+、Ninja、C++20 编译器、Python、Vulkan
headers/loader/ICD 和 `glslc`；真机满足当前 Compute/TaskPublication/Timeline 路径需要的
features。本包不擅自安装依赖。记录 `uname -a`、`glslc --version`、`vulkaninfo --summary`、
完整 driver/device/feature 信息与 validation layer 开关；软件 ICD 不算目标真机证据。

```sh
# 前置：正常 Linux preset 基线；输出和完整测试映射由运行者归档。
cmake --preset dev-vulkan
cmake --build --preset dev-vulkan -j4
ctest --preset dev-vulkan --show-only=json-v1
ctest --preset dev-vulkan --output-on-failure
build/dev-vulkan/vg-platform-probe --validate

# G4 独占新目录，避免重配置别的代理目录。
cmake -S . -B build/g4-vulkan -G Ninja -DBUILD_TESTING=ON -DVG_ENABLE_VULKAN=ON -DVG_ENABLE_METAL=OFF -DCMAKE_BUILD_TYPE=Debug -DVG_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/g4-vulkan -j4
ctest --test-dir build/g4-vulkan --show-only=json-v1
ctest --test-dir build/g4-vulkan --output-on-failure
ctest --test-dir build/g4-vulkan -R '^(conformance.device-hal.vulkan|vertical-slice.vulkan(\.task-tier0|\.timeline|\.raster-rejected|\.raster-msl-rejected)?|api.mixed-domain.vulkan)$' --repeat until-fail:20 --output-on-failure

cmake -S . -B build/g4-vulkan-off -G Ninja -DBUILD_TESTING=OFF -DVG_ENABLE_VULKAN=ON -DVG_ENABLE_METAL=OFF -DCMAKE_BUILD_TYPE=Debug -DVG_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/g4-vulkan-off -j4
python3 tests/tools/check_build_boundary.py build/g4-vulkan build/g4-vulkan-off
build/g4-vulkan-off/vg-platform-probe --validate
build/g4-vulkan-off/vg-offscreen-triangle-ppm build/g4-vulkan-off/g4-reference-smoke.ppm

python3 tests/vertical_slice/vulkan_capability_contract_test.py .
python3 tests/tools/test_schema_generator.py . build/g4-vulkan/generated
python3 tests/tools/check_core_headers.py --generated-dir build/g4-vulkan/generated --baseline-ref 99ec414
python3 tools/vg-docs/vg_docs.py .
git diff --check
```

每轮测试使用独占 `TMPDIR`（由 `mktemp -d` 创建），不要并发共享 capture 固定临时文件。
Linux 同构建身份先用 `check_compiler_sources.py <build-dir> --record <baseline.bin>` 记录，
搬迁后 `--compare`，不能拿另一个编译器的二进制文件布局冒充 source 守恒。

预期：全量名称/命令/属性与 Linux 前置基线一致；全量总数在 Linux configure 后记录，不杜撰。
上述定向 regex 按当前 source 登记应选择 7 项，20 次即 140 次；必须先核查 CTest 实际选择，
不得将 0 selected 或 skip 计为通过。Vulkan 无设备测试失败不能绕成成功。
全量同时核查 report 与已执行命令，未广告 Raster 必须 Unsupported 且不产生部分执行。

OFF 预期所有目标完整链接、CTest 为零、生产 archive 无 tests/harness，ON/OFF 生产源集合
一致。PPM 是 Reference/公共 ABI 链接 smoke，**不是 Vulkan Raster 证据**；Vulkan 设备证明
来自 probe 身份和上述 driver-dependent conformance，不能让默认 Reference 回退替代它。
Vulkan validation 若外部 layer 可用应单独启用运行并保存未分类错误；capability 字段
`validation_available` 本身不证明 layer 真正启用。没有 layer 时如实列缺口。

静态验收：生产不得 include tests/support；四个实验入口不再出现在 DeviceHal 门面；旧
singleton/`per_node_packages[0]`/`resolved_nodes[0]`/backend effect/order 重建零命中；每个
旧函数唯一 owner；generated ring 常量仍是唯一来源。生产 source 中历史平台限制评论只在
相关搬迁处修正/移入本报告，不能批量改写历史 ADR 或以注释代替真实能力。

## 8. 本轮四级状态

| 层级 | 状态 |
|---|---|
| G4 源码实施 | 未开始，受已冻结平台门禁约束 |
| 本机核查 | 环境、现有 owner/依赖、静态路径与实际测试归属已核查 |
| 平台验证 | Linux SDK 构建与 Vulkan 真机均未执行；awaiting-platform |
| 文档 | 迁移/寿命/测试/命令清单已交付；不是平台通过证据 |

下一步需要的是明确可用、授权的 Linux GPU 入口与基线，而不是继续在本机重跑 stub。

本轮文档完成后运行 `python3 tools/vg-docs/vg_docs.py .`（成功，仅三条已有外部来源
未检查 warning）、`python3 tests/vertical_slice/vulkan_capability_contract_test.py .`
（通过）、`git diff --check`（通过）。Vulkan 生产文件与两份测试无工作树修改；两个 G4
空 fragment 是统筹预先创建，本代理没有编辑。上述均不是 Linux 编译或 GPU 执行。

## 9. 用户调整顺序后的源码实施（2026-09-03）

用户要求先在 M 芯片开发机完成 G4，稍后到其找到的远程测试机器验证。统筹修订了
[执行冻结](g3-g4-orchestration.md)，因此本阶段已搬迁源码；Linux SDK/GPU 从开工前置
改为最终验收门禁，没有降低 Unsupported、生命周期或平台证据标准。

### 9.1 拆分前快照

动源码前保存于忽略目录 `build/g4-baseline/`：

| 文件 | SHA-256 |
|---|---|
| `vulkan_device_hal.cpp` | `1d19893e16293276ebee87e2f54d59c291fe184ab5b65996b56cd72a36010727` |
| `vulkan_device_hal.h` | `d4739510a624c88154d1ac0cb2d65e08097fe261f41f6a0514fd18da433d0728` |
| `vulkan_capability_contract_test.py` | `3a4f862ad2c4a9cb2f5f5fb65984ecbbc8bad26270cdf09f5421c133a057ff81` |

基线 HEAD 仍为 `99ec4148bfd0fdf91557c200b6bfb05397c1bed3`。
`function-owners.json` 记录 64 个原顶层函数的原行号、目标 owner 与完整函数 SHA-256；
`split.py` 保存初始机械迁移步骤，后续声明/include/静态检查修订在工作树 diff 中；
`check_conservation.py` 是只读复核工具。不要在已修改工作树重跑 one-shot splitter。
完整前序恢复点仍为 `build/g12-checkpoint/g12.patch`；这些都是本地文件，不是已推送备份。
远程验证须同时取得这些基线工件与拆分后源码身份，避免用不同既有状态比较回归。

### 9.2 已实施文件与边界

§3.1 的 64 个函数均有唯一映射，函数体只发生 owner 限定/借用 state 的机械替换；
facet guard 的构造、begin、析构按原 RAII 配对定义在 resources。

| 实际文件 | 已实施职责 |
|---|---|
| `src/backends/vulkan/vulkan_device_hal.h/.cpp` | SDK 无关薄门面；唯一 `unique_ptr<detail::DeviceState>`；create_impl 仍精确选择 UUID 与协商能力，compile/submit 只委托 |
| `src/backends/vulkan/vulkan_device_internal.h` | 私有 state、原 record/cache、内部函数声明；只有原小型 tuple 比较/format selector 保留内联，不含巨型实现 |
| `src/backends/vulkan/vulkan_physical_types.h` | 已向统筹报告的必要共用私有头；原五个 physical DTO/枚举逐字迁移，生产无需 include tests |
| `src/backends/vulkan/vulkan_resources.cpp` | buffer/image/view/sampler/descriptor pool、ring临时buffer、物理representation、facet use guard、唯一 state 析构 |
| `src/backends/vulkan/vulkan_pipelines.cpp` | 原 package/entry cache key、glslc 子进程、compute/ring/sample/storage/raster pipeline、原 storage shader source |
| `src/backends/vulkan/vulkan_lowering.cpp` | 原 Stage 6 与 representation 可表达性、wave transition lowering |
| `src/backends/vulkan/vulkan_commit.cpp` | 原 Stage 7 顺序完整保留：precheck/continuation/holds/representation/schedule/dispatch/writeback/publication |
| `src/backends/vulkan/vulkan_encoding.cpp` | command/fence/timeline、真实 Task shape dispatch、sync2、publication pass |
| `src/backends/vulkan/vulkan_raster.cpp` | 原物理 raster pass 改为 private `run_raster_pass`；原未广告 Raster 的拒绝保留 |
| `src/backends/vulkan/vulkan_diagnostics.h/.cpp` | 原 set_error/make_facet_report；不持有 state、不推导命令数 |
| `tests/support/vulkan_adapter_harness.h/.cpp` | 原 sample/storage/classification 方法借用同一个 device state；raster 方法调用原物理 pass；无新 public HAL 虚接口 |
| `cmake/g4-vulkan-sources.cmake` | 显式新增七个生产 .cpp，facade 不重复登记，ON/OFF 同源 |
| `cmake/g4-vulkan-tests.cmake` | 测试专用 `vg_vulkan_adapter_harness` 静态库与既有 target 链接；没有新增 CTest 或改 oracle |
| `tests/vertical_slice/vulkan_capability_contract_test.py` | 分别读取真实 owner 的函数体；所有原 capability、Node、schedule、continuation、shape、publication 断言保留，另检查薄门面与生产不反向依赖 tests |

没有修改 root CMake、Core/Compiler/API/ABI/schema、Vulkan probe、既有 Vulkan C++ 测试或
G3 文件。没有开始 G5 测试分轨、开放 Raster 或其它 capability。`run_*` 四个物理入口
仍没有实际外部测试调用，本包将其编译归入 harness，不声称补齐了 GPU 覆盖。

### 9.3 本机验证与当前集成缺口

使用独立 `build/g4-reference`，Debug + ASan/UBSan；每轮 CTest 使用 `mktemp -d` 生成的
独占 TMPDIR，不重配别的代理 build 目录。

| 验证 | 实际结果 |
|---|---|
| 64 原顶层函数体守恒 | 通过；只规范化 owner/state 借用，不改变控制流、literal、Vk 调用顺序 |
| 19 原类型/枚举 | 定义逐字一致且唯一 owner；包括全部 nested records 与 physical/harness DTO |
| 八个生产 TU + 一个 harness 非 `VG_HAS_VULKAN` 语法 | 通过；只是 stub 分支 |
| 五个门面/私有/物理/诊断/harness 头自包含 | 非 Vulkan 配置通过；不代表 SDK native 头验证 |
| 上述九个 TU 与 Reference/Core/Compiler/IR 实际链接 | `/usr/bin/c++` + ASan/UBSan 成功；stub smoke 返回明确 unavailable，不创建 GPU |
| Reference 全新 configure/build | 129 步构建成功 |
| Reference 全量 | **37/38**，16.33 秒；唯一失败为共享 `schema.generate` 旧 Vulkan 路径；其余无 skip |
| Vulkan source-contract | 通过；不替代 SDK/GPU |
| docs/diff | docs 检查通过（原有三条外部来源 warning），`git diff --check` 通过 |
| Linux SDK、真实 Vulkan、Vulkan ON/OFF | 未执行，远程最终门禁 pending |

stub 首次链接误用了 PATH 中另一版本 clang，与 CMake `/usr/bin/c++` 编译的 ASan runtime
不一致，出现 `___asan_version_mismatch_check_apple_clang_2100`；改用构建记录中的
`/usr/bin/c++` 后通过。这是工具链身份修正，不是源码或 sanitizer gate 放宽。

**共享文件集成项已交给统筹，G4 未越权编辑：**
`tests/tools/test_schema_generator.py` 目前仍在旧 `vulkan_device_hal.cpp` 查找
`kTaskRingDispatchXWord`。正确 owner 为 `vulkan_resources.cpp`（ring buffer sizing）；
禁止 `pack_task_record`/`unpack_task_record` 的断言应逐个扫描全部新 Vulkan 生产 owner。
统筹适配后必须重新跑 schema 和 Reference 全量至 38/38；未完成前不能称“本机全绿”。

主要可复现命令：

```sh
python3 build/g4-baseline/check_conservation.py
cmake -S . -B build/g4-reference -G Ninja -DBUILD_TESTING=ON -DVG_ENABLE_VULKAN=OFF -DVG_ENABLE_METAL=OFF -DCMAKE_BUILD_TYPE=Debug -DVG_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/g4-reference -j4
vg_g4_tmp=$(mktemp -d /private/tmp/vg-g4-reference.XXXXXX)
TMPDIR="$vg_g4_tmp" ctest --test-dir build/g4-reference --output-on-failure
python3 tests/vertical_slice/vulkan_capability_contract_test.py .
python3 tools/vg-docs/vg_docs.py .
git diff --check
```

stub 命令不设置任何 Vulkan 宏或伪造 SDK 头：

```sh
/usr/bin/c++ -std=c++20 -fsanitize=address,undefined -g -Isrc -Iinclude -Ibuild/g4-reference/generated build/g4-baseline/stub_link_smoke.cpp src/backends/vulkan/vulkan_device_hal.cpp src/backends/vulkan/vulkan_resources.cpp src/backends/vulkan/vulkan_pipelines.cpp src/backends/vulkan/vulkan_lowering.cpp src/backends/vulkan/vulkan_commit.cpp src/backends/vulkan/vulkan_encoding.cpp src/backends/vulkan/vulkan_raster.cpp src/backends/vulkan/vulkan_diagnostics.cpp tests/support/vulkan_adapter_harness.cpp build/g4-reference/libvg_backend_reference.a build/g4-reference/libvg_core.a build/g4-reference/libvg_compiler.a build/g4-reference/libvg_ir.a -o build/g4-baseline/stub-link-smoke
build/g4-baseline/stub-link-smoke
```

### 9.4 当前四级状态与交接

| 层级 | 当前状态 |
|---|---|
| 源码实施 | 已完成冻结范围的拆分与 harness/source-contract/CMake fragment 迁移 |
| 本机验证 | 守恒、stub 语法/链接、静态通过；37/38，等待统筹共享 schema 路径适配后全量重跑 |
| 平台验证 | Linux SDK/GPU/ON-OFF 与物理 harness 覆盖缺口仍待远程验证，不宣称整体 G4 通过 |
| 文档与独立审查 | 当前结果已记录；待统筹独立复核，无 commit/push |

远程按 §7 对拆分前后分别执行，核查完整 NodeRef cache、Task shape、effect/wave同步、
continuation 拒绝无副作用、hold/epoch/ConsumeInput、Unsupported 与真实报告计数。物理
harness 新测试矩阵仍须单独冻结，不通过修改 capability 使旧拒绝路径“成功”。
