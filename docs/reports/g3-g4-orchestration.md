# G3/G4 后端拆分执行冻结

日期：2026-09-03。用户要求 G1/G2 复核无问题后启动 G3/G4。
统筹验收见 [G1/G2 集成记录](g1-g2-orchestration.md)：Reference38/38、真实Metal72/72，
ON/OFF生产边界及Core/Compiler守恒通过。既有capture.view共享临时文件风险留G5。

## 基线与共享文件

- main HEAD 99ec414；G0/G1/G2仍未提交。启动前完整tracked/untracked patch保存在
  build/g12-checkpoint/g12.patch，已反向check验证；这是本地恢复点，不是远程备份。
- 新工作保留全部前序变更，不切分支、不commit/push、不撤销他人代码。
- 根CMake唯一owner仍为统筹。为免子代理等候源码登记，预接入四个显式fragment：
  cmake/g3-metal-sources.cmake、g3-metal-tests.cmake 归G3；
  cmake/g4-vulkan-sources.cmake、g4-vulkan-tests.cmake 归G4。
  production fragment只target_sources新增实现，不重复登记原facade；test fragment在既有tests
  登记之后、BUILD_TESTING+对应backend开启时加载，只定义/链接窄harness，不改CTest合同。
- 各自使用build/g3-metal、build/g4-vulkan及对应OFF新目录；各自测试独占TMPDIR，
  不并发重配dev-*、g0-*、g1-*、g2-*目录。无新runtime/build框架，不是提前实施G6。

## G3：Metal（可实施）

原始文件 src/backends/metal/metal_device_hal.h/.mm；metal_probe.mm只在头拆分导致必要include
适配时可改，不改capability。拟新增同目录 metal_device_internal.h、metal_resources.mm、
metal_pipelines.mm、metal_lowering.mm、metal_commit.mm、metal_encoding.mm、metal_raster.mm、
metal_diagnostics.h/.mm；统筹根据G3职责清单补充批准 metal_physical_types.h（生产与harness
共用的既有物理描述）、metal_representation.mm（物理transform）、metal_shader_sources.h/.mm
（原storage内嵌source）。这些不是新增语义或公共接口。

顺序：私有状态声明 → resource/cache → pipeline/lowering → encoding/commit → diagnostic/harness。
facade保留Device创建/能力/compile/submit入口；private header只声明，不搬成巨型内联实现头。
所有cache、MTL retain/release、析构和错误清理owner保持唯一。

run_*_facet、run_raster_triangles、run_representation_transform、cull/classification、
显式Tier1/indexed实验入口归 tests/support/metal_adapter_harness.h/.mm。
生产需要的run_raster_pass/representation physical操作留backend，由harness复用；
生产代码不得include tests/，不扩大公共hal::DeviceHal虚接口。metal_tier2.*保持G0支撑库归属，
不重写算法。诊断计数在真实命令位置产生，diagnostics只汇集/格式化。

允许同步适配：tests/vertical_slice/metal_task_timeline_test.cpp 的harness调用/相关类型include，
不重构测试函数或改变断言；tests/tools/test_schema_generator.py 的Metal owner文件读取路径，
全部原断言保留。另允许metal_identity_scene_root_cache_test.cpp仅增加
metal_physical_types.h的include以取得既有RasterVertex，不改用例或oracle。
其他测试保持不动，需要新增调用点清单则先报告统筹。

DoD：职责/符号迁移表、唯一owner/无生产反向测试依赖；原执行顺序、NodeRef→pipeline、
Task shape、schedule/effect/transitions、report事件/计数、continuation/hold/representation/completion
不变；Reference38、真实非沙箱Metal72全量，无skip；mixed/Node-aware/lifetime/ConsumeInput/
cache相关回归、独立OFF构建链接与ABI smoke；G0/G1/G2显式门禁通过。
本包只改变harness链接，不拆分monolith测试或改变CTest名称/参数/属性（G5）。
文档只写 docs/reports/g3-metal-split.md，不同时改总报告/START/ADR/前序台账。

## G4：Vulkan（按用户新安排先实施，远程验证后验收）

**用户明确调整（2026-09-03）：** 用户已找到需要远程连接的测试机器，要求本机先按规划
实施G4，不等待Linux/Vulkan验证环节。此决定修订G0及本文件先前的开工顺序：Linux SDK/
真实Vulkan由“源码搬迁前置门禁”改为“最终验收门禁”，不降低验证标准。
原G4子代理恢复实施以下冻结范围，无需再次等待Linux入口即可搬迁源码/适配测试与CMake。

统筹本机为Darwin，未发现glslc/vulkaninfo；Docker CLI存在，但daemon socket不存在。
不得启动/安装Docker、拉取镜像、搭建服务器、猜SSH目标或扫描凭据来绕过缺失环境。
远程地址/checkout/连接方式尚未提供时，不自行尝试连接，不以缺少连接信息停止本机实施。

远程阶段须分别验证G4拆分前快照和拆分后结果，保存环境、source身份与测试映射，
区分既有缺陷与拆分回归；前序build/g12-checkpoint/g12.patch包含未拆分的Vulkan基线。
本机完成可做的Reference、结构/符号守恒、stub语法及source-contract检查，只能记录
“源码实施/本机验证完成，Linux SDK/GPU验证pending”，不得宣布G4整包验收通过。

计划范围：src/backends/vulkan/vulkan_device_hal.h/.cpp、probe.cpp必要include；
拟新增 vulkan_device_internal.h、vulkan_resources.cpp、vulkan_pipelines.cpp、
vulkan_lowering.cpp、vulkan_commit.cpp、vulkan_encoding.cpp、vulkan_raster.cpp、
vulkan_diagnostics.h/.cpp；tests/support/vulkan_adapter_harness.h/.cpp；上述两个G4 fragment。
tests/vertical_slice/vulkan_task_timeline_test.cpp仅必要harness调用适配；
vulkan_capability_contract_test.py按真实owner迁移原断言，不靠注释或全文拼接假通过。
共享test_schema_generator.py归G3/统筹，G4只报告Vulkan读取路径调整需求，不并发修改。

统筹复核确认补充 `vulkan_physical_types.h`：五个既有共用物理 DTO/枚举的唯一 owner，
定义逐字不变；避免生产代码反向 include harness，不属于新增语义或公共接口。

保留per-Node cache、完整NodeRef、task_order/schedule、真实vkCmdDispatch shape与同步；
ring只publication；未广告Raster仍整计划Unsupported；不扩大capability、不新造submit。
Linux编译、真机全量、OFF与capability/report门禁全部有证据后才可关闭，不用stub/语法检查替代。
G4报告已发现四个纯物理入口没有既有外部调用测试；移入harness及可编译不代表GPU覆盖。
本轮不擅自新增物理测试矩阵、开放Raster或改oracle，明确列入远程验收覆盖缺口待单独冻结。

## 不变量与状态

两包依据整改报告§9.3/10.1、START、ADR-053/054、原设计Stage0—7；不动Core/Compiler/
API/public ABI/schema，不改ordering/poison/continuation/lifetime，不新增公共资源或生命周期。
只按既有职责搬迁，不以文件短或局部绿灯作为完成标准。
子代理不得再启动子代理，完成后由统筹独立复核。

| 包 | 当前状态 | Owner职责 |
|---|---|---|
| G3 | 实施、本机/真实Metal验证及统筹独立审查通过 | 不代表G5/G6完成 |
| G4 | 源码与本机集成审查通过，远程最终验收pending | Linux SDK/GPU、ON/OFF及物理覆盖仍待补证 |
| 集成 | 统筹共享schema路径适配完成；Reference/Metal全绿 | 未提交、未推送；保留全部前序工作树变更 |

## 统筹独立复核（2026-09-03）

结论：未发现本次拆分引入的代码阻塞项；G3可关闭，G4只能关闭源码实施与本机验证，
不能关闭平台最终验收。没有扩大到新的语义修复、G5/G6、Vulkan Raster或新测试矩阵。

重新索引工作树为 `vg-api-g34-review`，图未完整覆盖Objective-C++，以源码/调用点补证。
先验证代理保存的原始Metal/Vulkan源与HEAD99ec414字节摘要一致，再复跑搬迁守恒检查：
Metal109个原函数、21个原公共数据/枚举，Vulkan64个原函数、19个类型/枚举均保持唯一
owner；设备/cache声明与初始值不变。人工核查门面委托、唯一私有state、析构/失败清理、
Stage6/7顺序、NodeRef绑定、真实shape/sync、report与harness非拥有关系。

唯一集成改动是统筹负责的 `tests/tools/test_schema_generator.py`：ring sizing读
`vulkan_resources.cpp`，禁止本地codec逐个覆盖所有Vulkan `.cpp`及迁出的harness。
没有删除断言、改变schema或拼接成假旧文件。代理G4的37/38是此适配前的历史结果；
本次集成后已为38/38。

| 统筹亲自执行的门禁 | 结果 |
|---|---|
| dev-reference / dev-metal重新构建 | 通过，Debug + ASan/UBSan |
| Reference全量 | 38/38，17.02秒，无skip |
| 非沙箱真实Metal全量 | 72/72，34.08秒，无skip |
| 12项关键回归各重复20次 | 240/240，44.45秒，无skip；独占TMPDIR |
| g3-off-reference / g3-off-metal全目标构建 | 通过；ON/OFF生产源41/52，OFF测试均为0 |
| G0测试映射 | Reference/Metal摘要与冻结基线一致，未改变CTest合同 |
| OFF probe + 公共ABI PPM smoke | 通过；Metal为Apple M1 / Version 26.6.2 (Build 25G83)，PPM不作Vulkan证据 |
| G1显式门禁 | 64类型、1409实现行守恒；16头自包含/无环 |
| G2显式门禁 | 24567字节与build/g2-reference/before.bin一致 |
| 新后端头自包含 | 九个C++头、一个Objective-C++私有头通过；Vulkan仅非SDK配置 |
| Vulkan九TU实际stub链接 + ASan/UBSan smoke | 通过，明确unavailable；不是SDK/native或GPU验证 |
| 生产依赖/静态残留 | 无tests反向include、请求袋、first-Node投影、backend拓扑重建；Metal生产archive无AdapterHarness/Tier2 |

可复现主命令（全量与重复测试分别使用新的 `mktemp -d` TMPDIR；Metal在沙箱外执行）：

```sh
cmake --build build/dev-reference -j4
cmake --build build/dev-metal -j4
ctest --test-dir build/dev-reference --output-on-failure
ctest --test-dir build/dev-metal --output-on-failure
ctest --test-dir build/dev-metal -R '^(api.multicode-taskgraph-conformance|api.mixed-domain.(reference|metal)|core.execution-plan|conformance.device-hal.metal|vertical-slice.metal|vertical-slice.metal.(identity-scene-root-cache|mixed-domain|effect-dag|consume-input|representation-churn|checked-facet-generation))$' --repeat until-fail:20 --output-on-failure
python3 build/g3-baseline/check_migration.py
python3 build/g4-baseline/check_conservation.py
python3 tests/tools/check_build_boundary.py build/dev-reference build/g3-off-reference
python3 tests/tools/check_build_boundary.py build/dev-metal build/g3-off-metal
python3 tests/tools/check_core_headers.py --generated-dir build/dev-reference/generated --baseline-ref 99ec414
python3 tests/tools/check_compiler_sources.py build/dev-reference --compare build/g2-reference/before.bin
```

G4剩余门禁不是本轮新增代码缺陷：需在授权远程Linux机器对拆分前后分别进行SDK构建、
真实Vulkan全量、ON/OFF边界、capability/LoweringReport核对；可用时启用validation layer。
四个既有纯物理入口没有实际外部测试调用，静态库编译/链接不证明执行覆盖，须先单独冻结
远程最小smoke范围。远程连接信息未提供，未自行连接或配置环境，也未启动后续工作包。
