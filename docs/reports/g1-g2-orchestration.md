# G1/G2 并行执行冻结

日期：2026-09-03。Owner：统筹代理；用户已批准启动 G1/G2 子代理并行实施。

## 基线与集成

- 当前 main HEAD 为 `99ec414`；G0 已验证变更仍未提交，必须保留。
- 在子代理启动前，将完整 G0 tracked/untracked diff 保存到忽略目录
  `build/g0-checkpoint/g0.patch`，形成可恢复的本地集成检查点；这不是 Git 提交或远程备份。
- G0 Reference 38/38，真实 Metal 72/72，重复 120/120，ON/OFF 边界通过。
- 两代理共享 main 工作目录，不切换分支、不 commit/push，不撤销别人的变更。
- 仅统筹代理可修改根 CMakeLists.txt；子代理尽早报告完整源文件清单与准备登记时机。
- 子代理使用独立 build/g1-reference、build/g2-reference 目录；不得并发重配置同一构建目录。
- 总结分别写 g1-core-split.md、g2-compiler-split.md；不同时修改 START、ADR、整改总报告或 G0 台账。

## G1 冻结

原始文件：src/core/core.h、core.cpp；现有 execution_plan.h/.cpp、execution_schedule.h/.cpp、
scene_root.h/.cpp、task_schema.h/.cpp 仅允许必要的 include 适配，不重写其逻辑。

拟新增 bounded contexts（均在 src/core）：resource_types.h；arena.h/.cpp；facet.h/.cpp；
representation.h/.cpp；pointer_graph.h/.cpp；effect_graph.h/.cpp；task_graph.h/.cpp；
access.h/.cpp；node.h/.cpp；envelope.h/.cpp；execution_result.h/.cpp。
必要的命名/依赖边界调整须先向统筹报告，不另起公共模型。

保留 core.h 聚合兼容；新子模块不 include core.h，不新增万能 internal header；不改变类型布局、
public declaration、NodeRef identity、Arena/Facet hold、epoch/consume、graph/order、fault/certificate/
continuation 合同。core.cpp 迁空后可删除，由统筹移除源码登记。

不修改 compiler、backend、API、include/vg、schemas、tests 用例、runner；不进行 G5 测试拆分。
允许新增 tests/tools/check_core_headers.py，作为独立显式执行的头文件自包含/依赖检查，不注册或改写既有 CTest。

验收：原定义有逐项唯一 owner；兼容 include 可编译；新头自包含且无循环依赖、HAL/public ABI 引入；
Reference 全量、Core/lifetime/effect/certificate/continuation/schedule 回归；统筹完成真实 Metal 和 ON/OFF。

## G2 冻结

原始文件：src/compiler/compute_package.cpp、compiler.h；compiler.cpp 仅必要 include 调整。
拟新增 compute_package.h、compute_codegen.h/.cpp、shader_sources.h、shaders/task_ring.cpp、
shaders/facet.cpp、shaders/raster.cpp、shaders/cull_compact.cpp（均在 src/compiler）。

compiler.h 保留兼容声明入口；package 校验/bindings/hash/source-map 组织与指令 source 发射分开；
独立 shader-source 函数按既有职责搬迁。compute_task_ring.*、pipeline_classification.* 已独立，不重做。

不改 shader 字节输出、package hash/source-map/bindings、诊断、schema/golden、public ABI，
不改 backend 内嵌 shader，不引入运行时 shader 文件读取、模板框架或新生成器。
不修改 Core、backend、API、schemas、tools/vg-schema、CMake、既有测试用例；必要新检查限
tests/tools/check_compiler_sources.py（如需要），独立执行不改变既有 CTest。

修改前记录 source/package 输出基线；需涵盖 linear/pointer/indexed、ring、facet guard/sample、
raster、cull 两端 source，不能只靠覆盖少数包的 golden。修改后逐字节/字段比对。
验收：上述比对、现有 package/ring/golden/schema tests、Reference 全量、自包含头检查；统筹完成真实 Metal 和 ON/OFF。

## 共用门禁与状态

按整改报告 §9.3/§10.1、remediation-orchestration-memory 与 START 规范；先读再改。
38/72 是当前完整测试总数；不改 CTest 名称/命令/属性/skip/oracle。不用局部绿灯宣告跨平台完成。
Linux SDK/Vulkan 真机 pending 不改变；不扩大成 G3—G6。最终报告列文件/职责迁移表、命令/结果、
未验证项及实现/本地验证/平台验证/文档四级状态。

| 包 | Owner | 状态 | 后续 |
|---|---|---|---|
| G1 | g1_core_split | 实现与独立目录 Reference 验证完成；统筹复核通过 | 后续 G5 不在本包实施 |
| G2 | g2_compiler_split | 实现、输出守恒与独立目录 Reference 验证完成；统筹复核通过 | 后续 backend 拆分仍须保持输出与schema合同 |
| 集成 | 统筹代理 | 根 CMake、真实 Metal/ON-OFF 与复核完成 | Linux 门禁不关闭 |

## 统筹复核与集成（2026-09-03）

本轮用户要求检查 G1/G2，无问题后启动 G3/G4。子代理遵守中央文件独占权限，
最初交付尚缺根 source 登记、schema 路径适配；统筹补齐后才接受工作包。

- 根 CMake：core.cpp 替换为十个 owner .cpp，compiler 增加五个新 .cpp。
- tests/tools/test_schema_generator.py：SceneRoot 宏检查改读 shaders/raster.cpp，
  ring fragment/禁止硬编码检查改读 shaders/task_ring.cpp；原断言均保留，没有假注释占位。
- docs/issue/phase-f-and-runtime-risks.md：TaskGraphBuilder::seal 链接跟随新 task_graph.cpp。
  首次统筹 Reference 为37/38，唯一 docs.check 断链；修正后重新全量38/38。
- 统筹通过新图索引、源码核查及可复现门禁确认 Core 64 类型逐字一致、唯一 owner，
  1409 实现行守恒，16头自包含/重复包含/无依赖环；GraphEpochBuilder 的 reorder warning
  原基线已有，不为消除warning改初始化或布局。
- 统筹重跑 G2 输出矩阵：24567字节与 before.bin 完全一致，摘要仍为
  dee036f482a910f9af0698817a241689c45a1d26f8321585796c4b7a659cffbd。
- 统筹 dev-reference 全量38/38（17.56秒）、非沙箱真实Metal72/72（51.01秒），无skip；
  build/g0-off-reference 和 build/g0-off-metal 全目标构建/链接成功，生产编译单元41/43，
  check_build_boundary 两profile通过，CTest name/command/properties 摘要仍与G0一致。
- 两个OFF产物均通过公共C ABI离屏绘制；Metal probe实际枚举Apple M1。
- G1 独立目录全新129步构建、隔离TMPDIR后38/38（8.48秒）；G2独立目录129步构建、
  38/38（23.19秒）及定向4/4，均无skip。详见两包报告。
- G1 首次并发测试暴露既有capture.view固定系统临时文件碰撞；未改用例，独占TMPDIR重跑通过。
  后续并行测试使用隔离临时目录，测试代码临时路径治理留给G5。

审查结论：限定于G1/G2的职责与行为保持目标，无剩余阻塞项；不是G3/G4或整个§9.3完成声明。
实施、本地验证、Reference/真实Metal平台验证与文档闭合；Linux SDK/GPU仍pending。
本轮不commit/push，以完整本地补丁检查点隔离已验证状态，再开始后续包。
