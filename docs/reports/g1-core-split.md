# G1：Core bounded-context 文件拆分

日期：2026-09-03。Owner：G1 Core 子代理；中央 CMake、跨包集成与真实 Metal 验证由统筹负责。
当前状态：实现与独立 Reference 完整构建/回归完成；跨包平台验证与独立复核由统筹负责；Linux pending。

## 冻结边界

- 依据 START、原设计 01/02/03/11/12、整改报告 §7—10（特别是 §9.3、§10.1）、
  remediation-orchestration-memory，以及 [G0 基线](g0-build-baseline.md) 和
  [G1/G2 并行冻结](g1-g2-orchestration.md)。
- 工作在共享 main / `99ec414` 上；保留已有 G0 和并行 G2 变更，不切分支、不提交或推送。
- 只迁移原 core.h/core.cpp 的既有类型及实现，不改变布局、字段、函数声明或行为。
- 不改变 Stage 0—7、Arena 生命周期、Facet hold、Node authority、Task publication、epoch、
  certificate/lease、effect、continuation、fault/poison；不增加公共对象或 submit 原语。
- 不修改 compiler/backend/API/public ABI/schema、既有测试 oracle/登记、runner；不开展 G3—G6。
- execution_plan、execution_schedule、scene_root、task_schema 已有算法不改，仅调整其头文件 include。
- 中央 CMake 由统筹唯一管理；G1 只交付新源文件清单。所有新文件都位于冻结清单内。

## 唯一 owner 迁移表

下表相对于仓库根；除明确标为既有的文件外均为新增文件。

| 新 owner（src/core/） | 原 core.h/core.cpp 的内容 | 依赖边界 |
|---|---|---|
| resource_types.h | ObjectState、PoisonState、ValidationProfile、PointerRef、RepresentationRef、FacetRef、AddressDomain | 仅 cstdint；无服务对象 |
| arena.h/.cpp | Allocation、Arena 及其全部 allocate/lookup/copy/retire/hold/transform/consume/import 操作 | 头依赖 resource_types、前置声明 ConsumeProof；实现消费 representation |
| facet.h/.cpp | FacetKind/format/dimension/filter/wrap/swizzle、CanonicalView、RasterFacetPair、FacetSlot/Status/Pool、bytes_per_texel、validate_facet_target | 头依赖 resource_types、前置声明 Arena；实现消费 arena |
| representation.h/.cpp | ConsumeProof、RepresentationEpoch/Builder | 头依赖 resource_types、前置声明 Arena/FacetPool；实现消费两者 |
| pointer_graph.h/.cpp | GraphEpoch/Builder、Edge、PointerGraph/Builder、局部 pointer_ref_equal | 头依赖 resource_types、前置声明 Arena；实现消费 arena |
| effect_graph.h/.cpp | EffectEdge/Kind、EffectGraph/Builder、Shape/ForkJoin、分类/排序/HB 算法 | 只依赖既有 IR 与标准库，不依赖 TaskGraph |
| task_graph.h/.cpp | DepthCompareOp、TaskKind、Topology、TaskRecord、PublicationState/Slot/Ring、TaskGraph/Builder | 依赖 effect_graph 与 facet；publication 原子操作不改 |
| access.h/.cpp | Certificate、WitnessEntry/Diff、AccessWitness、AccessCertificate/Mode、DiscoveryResult、certificate/discovery/composition、WorkingSetBudget/Lease | 依赖 pointer_graph 与 IR；实现消费 arena；不引入 HAL |
| node.h/.cpp | CodeObject、NodeEntry、NodeTable、局部 token/key | 依赖既有 IR；process-wide token、Device-owned 表、锁与 immutable ownership 原样保留 |
| envelope.h/.cpp | TimelineGate/Timeline、EnvelopeOverflow/Disposition、ContinuationTable、ExecutionEnvelope | 依赖 access 与 node；不依赖 HAL plan |
| execution_result.h/.cpp | FaultRecord、ExecutionResult、局部 JSON serialization helper | 依赖 access/resource_types；JSON 只进入实现 |
| core.h（既有） | 11 个 owner 头的兼容聚合入口 | 无新类型/实现；子模块不能反向包含它 |
| execution_plan.h（既有） | 仅将 core.h 改为 arena/envelope/representation/task_graph 的直接 include | 原算法与字段零变更 |
| execution_schedule.h（既有） | 仅改为 effect_graph/facet 的直接 include | 原算法与字段零变更 |
| scene_root.h（既有） | 仅改为 arena/task_graph 的直接 include | 原算法与字段零变更 |
| task_schema.h（既有） | 仅改为 task_graph 的直接 include | 继续消费生成的 vg_task_root.h |

原 core.cpp 已删除，不留下空翻译单元或兼容 wrapper。统筹已在中央构建中将其替换为上述十个新 .cpp。
Arena 与 Representation 在实现层有调用，头文件通过引用和前置声明保持 DAG；这不是新的生命周期服务。
resource_types 只有七个已有基础值类型/枚举，不是另一个万能 internal 头。

## 路径分类与测试边界

- 上表全是 portable Core 生产 owner，不含 direct-adapter fixture。
- tests/tools/check_core_headers.py 是新增的显式只读结构门禁，不注册 CTest、不改变现有测试数量。
- 现有 assembler-driven、narrow physical harness、semantic-negative 测试均原样保留。
- Core unit 测试拆文件属于 G5，本包不实施。

## 实现守恒与静态证据

- 迁移时以原 core.cpp 与十个新实现的非空行多重集交叉核对：去除 include 和外层
  vg::core namespace 包装后，1,409 行（含原注释与匿名 namespace）完全相同，无丢失或复制。
- 原 core.h 的 64 个顶层 class/struct/enum 定义按平衡括号提取，与新 owner 逐字比较：
  全部相同，各出现一次，没有新定义。只新增必要的前置声明与标准库 include。
- 两项守恒现由 tests/tools/check_core_headers.py 的可选 --baseline-ref 模式复现：
  type_definitions/migration_errors 检查唯一类型 owner 与原始定义字节；implementation_lines
  检查实现行多重集。该有界检查适用于本次冻结迁移，不宣称是通用 C++ parser。
  另用内存注入重复 PointerRef 定义、额外实现行确认两类变更均会被拒绝。
- 原历史注释随所属定义迁移；特别是 CodeObject 上旧 v1.1 说明与后续 materialized 字段注释
  的历史冲突没有借本包改变语义或批量清理，应按对应 Node 文档 owner 后续统一。
- 16 个 Core 头逐个独立且重复包含编译成功；本地 include 图无环。
- Core 实现与叶子头不再 include core/core.h；无 backend/public vg.h 引入。
- 新增门禁的内存负例检查覆盖反向 umbrella、循环、public ABI、backend、缺失头五种拒绝，
  并确认合法 umbrella → leaf 图通过。
- 14 个 Core .cpp 使用 c++ -std=c++20 -fsyntax-only 全部通过；这不替代完整链接或平台回归。

## 验证命令与结果

已执行：

```sh
python3 tests/tools/check_core_headers.py --generated-dir build/dev-reference/generated
c++ -std=c++20 -Isrc -Ibuild/dev-reference/generated -fsyntax-only src/core/arena.cpp src/core/facet.cpp src/core/representation.cpp src/core/pointer_graph.cpp src/core/effect_graph.cpp src/core/task_graph.cpp src/core/access.cpp src/core/node.cpp src/core/envelope.cpp src/core/execution_result.cpp src/core/execution_plan.cpp src/core/execution_schedule.cpp src/core/scene_root.cpp src/core/task_schema.cpp
rg -n 'core/core.h|backends/|vg/vg.h' src/core
git diff --check
```

头检查 PASS（16）；所有 Core 翻译单元语法通过；静态搜索零命中；diff 检查通过。

中央登记后，已从独立新目录 build/g1-reference 完成以下构建和验证：

```sh
cmake -S . -B build/g1-reference -G Ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DVG_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/g1-reference -j4
g1_test_tmp=$(mktemp -d /tmp/vg-g1-ctest.XXXXXX)
TMPDIR="$g1_test_tmp" ctest --test-dir build/g1-reference --output-on-failure
python3 tests/tools/check_core_headers.py --generated-dir build/g1-reference/generated --baseline-ref 99ec414
```

| 检查 | 实际结果 |
|---|---|
| 全新 Reference configure/build | 129/129 构建步骤成功，最终目标链接成功；Debug、ASan/UBSan ON、Metal OFF |
| Reference 全量 | 38/38，8.48 秒，无 skip；含 Core/plan/lifetime/effect/schedule/certificate/continuation、API、Mixed-domain、schema、golden |
| 头与迁移门禁 | 16 头独立重复 include 通过；64 类型逐字一致且 owner 唯一；1,409 实现行守恒 |
| 文档/静态 | docs.check 在全量中通过；无 core/core.h 反向 include、backend/public ABI 引入；git diff --check 通过 |

首次未隔离临时目录的全量是 37/38（25.99 秒），唯一 capture.view 报 cannot read capture。
源码 tests/unit/capture_view_test.cpp:319—332 使用固定 temp_directory_path 下的
vg-e014-capture-view.json/.md 并在结束时删除，存在跨构建目录并行测试的碰撞风险。
因此重跑使用 G1 独占 TMPDIR（实际 /tmp/vg-g1-ctest.xalXba），38 项全部通过。
没有修改测试断言、登记、参数或 skip；测试临时命名治理属于 G5，未越界修复。

真实 Metal 72 项、ON/OFF 生产边界与跨包集成由统筹重跑，不冒用本包 Reference 结果。
Linux SDK/Vulkan 真机始终未验证，不以本机语法或静态合同替代。

## 四级状态与后续依赖

| 层级 | 状态 | 剩余责任 |
|---|---|---|
| 实现 | 完成 | 独立复核确认 |
| 本地验证 | 独立新构建、Reference 38/38、头/语法/守恒完成 | 无本地待执行项 |
| 平台验证 | Metal/ON-OFF 待统筹，Linux pending | 统筹/平台执行环境 |
| 文档 | 范围、迁移、首次失败与完整回归已记录 | 统筹记录跨包证据 |

G1 不自动解锁依赖它的 G5 测试拆分；须等待要求的验证、审查和 clean integration。
