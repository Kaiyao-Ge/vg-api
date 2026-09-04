# G5 测试分轨交付记录

日期：2026-09-03。状态：Core/Reference/Metal实施与本机平台验证完成，待统筹独立复核。
依据 g5-test-orchestration.md；main 99ec414，保留未提交 G0—G4。
仅 Core/Reference/Metal 分批 G5；Vulkan 平台仍 pending，不修改其测试。

## 1. 实施前基线

五个源文件及 CMake 原样拷贝保存在 build/g5-baseline；sources.sha256 固定原始身份，
reference-ctest.json / metal-ctest.json 保存原有38/72项 name/command/properties。
图索引 vg-api-g5 已重建；图未捕获所有块内 C++ 关系，以源码补证。

## 2. 冻结迁移映射

下表行号均指 build/g5-baseline 中的原文件。每个旧函数/块只有一个实现 owner；
保留原 main/CLI 的调用顺序和失败返回。声明头只含窄声明/既有小 DTO/常量，函数体在 cpp。
共享原始 Core Arena 仍由 main 持有到程序结束，discharged proof 显式传参。
两块包含纯Core前置断言的Reference场景保留场景内原先状态/顺序，不伪造plan送HAL。
semantic-negative函数留在execution-plan套件，不搬进direct-adapter路径。

### Core main 块

| 原行/块 | 类别 | 目标（tests/unit/core/） |
|---|---|---|
| 47–191 `test_arena_task_graph` | Core semantic + negative | `graph_semantics.cpp` |
| 193–329 `test_reference_task_timeline` | plan-driven; semantic cross-validation prelude | `reference_submit.cpp` |
| 331–414 `test_reference_access_certificate` | plan-driven; Core certificate prelude | `reference_submit.cpp` |
| 416–538 `test_facet_pool` | Core semantic + negative | `facet.cpp` |
| 540–719 `test_view_and_representation_epoch` | Core semantic + negative | `facet.cpp` |
| 721–822 `test_consume_proof` | Core semantic + negative | `representation.cpp` |
| 824–872 `test_physical_transform_fault` | explicit physical-fault boundary; assembled input | `physical_representation.cpp` |
| 874–913 `test_capture_consumed_representation` | capture/reference oracle | `representation.cpp` |
| 915–967 `test_physical_consume_after_retire` | explicit physical boundary; real assembler rejection before retire | `physical_representation.cpp` |
| 969–1017 `test_representation_backpressure` | Core semantic + negative | `representation.cpp` |
| 1019–1088 `test_facet_generation_table` | Core semantic + negative | `facet.cpp` |
| 1090–1324 `test_reference_representation_submit` | plan-driven; shared-state rejection matrix | `reference_submit.cpp` |
| 1326–1401 `test_lease_budget_overflow` | Core semantic + negative | `access_envelope.cpp` |
| 1403–1478 `test_certificate_composition` | Core semantic + negative | `access_envelope.cpp` |
| 13–42 两个representation assembly helper | 真实assembler fixture | `representation_fixture.cpp`（窄声明头同名.h） |
| main | 仅保持原调用序列/共享Arena与proof | 原`tests/unit/core_test.cpp`；`cases.h`声明 |

### ExecutionPlan 函数

| 原函数/行 | 类别 | 目标（tests/unit/execution_plan/） |
|---|---|---|
| `fully_capable_reference_snapshot` 30–47 | semantic-boundary（含必要正向对照/physical hold合同） | `validation.cpp` |
| `expect_missing_capability` 49–60 | semantic-boundary（含必要正向对照/physical hold合同） | `validation.cpp` |
| `sealed_requirement_plan` 62–68 | semantic-boundary（含必要正向对照/physical hold合同） | `validation.cpp` |
| `test_stage6_capability_preflight_rejects_without_weakening` 70–106 | semantic-boundary（含必要正向对照/physical hold合同） | `validation.cpp` |
| `test_validation_profile_matrix_and_reset` 108–165 | semantic-boundary（含必要正向对照/physical hold合同） | `validation.cpp` |
| `test_basic_execution_plan_validation` 1389–1394 | semantic-boundary（含必要正向对照/physical hold合同） | `validation.cpp` |
| `test_effect_conflicts_are_deterministic_or_reject_reverse_cycle` 176–204 | semantic-boundary（含必要正向对照/physical hold合同） | `node_effect.cpp` |
| `test_execution_plan_assembler_sound_counterexamples` 365–452 | semantic-boundary（含必要正向对照/physical hold合同） | `node_effect.cpp` |
| `test_validated_effect_graph_and_full_noderef_packages_are_sealed` 454–604 | semantic-boundary（含必要正向对照/physical hold合同） | `node_effect.cpp` |
| `test_certificate_and_access_witness_reject_partial_coverage` 206–241 | semantic-boundary（含必要正向对照/physical hold合同） | `access.cpp` |
| `test_bounded_pointer_graph_canonical_identity` 243–259 | semantic-boundary（含必要正向对照/physical hold合同） | `access.cpp` |
| `test_execution_plan_assembler_bounded_pointer_graph_access` 606–691 | semantic-boundary（含必要正向对照/physical hold合同） | `access.cpp` |
| `test_execution_plan_assembler_seals_access_planning` 693–779 | semantic-boundary（含必要正向对照/physical hold合同） | `access.cpp` |
| `test_representation_stage5_assembler_boundaries` 781–839 | semantic-boundary（含必要正向对照/physical hold合同） | `representation.cpp` |
| `test_representation_semantic_plan_is_sealed` 841–882 | semantic-boundary（含必要正向对照/physical hold合同） | `representation.cpp` |
| `test_consume_input_proof_rejections` 1396–1431 | semantic-boundary（含必要正向对照/physical hold合同） | `representation.cpp` |
| `test_submission_lifetime_hold_is_transactional_and_repeatable` 894–973 | semantic-boundary（含必要正向对照/physical hold合同） | `lifetime.cpp` |
| `test_submission_lifetime_hold_deduplicates_facets_and_backing` 975–1273 | semantic-boundary（含必要正向对照/physical hold合同） | `lifetime.cpp` |
| `test_representation_outputs_join_lifetime_after_physical_stage` 1275–1348 | semantic-boundary（含必要正向对照/physical hold合同） | `lifetime.cpp` |
| `test_reference_multi_node_runtime_pointer_fault_preserves_prefix` 277–340 | plan-driven | `reference_submit.cpp` |
| `test_reference_submit_releases_holds_on_success_and_repeat` 1350–1387 | plan-driven | `reference_submit.cpp` |
| `check_failed` 18–21 | fixture声明/实现 | `fixture.cpp` |
| `task` 167–174 | fixture声明/实现 | `fixture.cpp` |
| `canonical_module` 261–269 | fixture声明/实现 | `fixture.cpp` |
| `canonical_code_object` 271–275 | fixture声明/实现 | `fixture.cpp` |
| `assemble_representation_case` 342–354 | fixture声明/实现 | `fixture.cpp` |
| `published_graph` 356–363 | fixture声明/实现 | `fixture.cpp` |
| `rgba_view` 884–892 | fixture声明/实现 | `fixture.cpp` |
| main / CHECK | 调用序列原样；CHECK失败行为原样 | 原驱动 / `fixture.h`；`cases.h`声明 |

### Reference raster main 块与helper

| 原块/行 | 类别 | 目标（tests/unit/reference/） |
|---|---|---|
| 157–294 `test_sample_oracle` | direct Reference oracle | `facet_oracles.cpp` |
| 296–455 `test_storage_attachment_oracles` | direct Reference oracle | `facet_oracles.cpp` |
| 457–577 `test_raster_oracle` | direct Reference oracle | `raster_oracles.cpp` |
| 579–692 `test_facet_token_oracles` | direct Reference oracle + negative | `facet_tokens.cpp` |
| 694–871 `test_builtin_raster_submit` | real assembler plan-driven | `plan_submit.cpp` |
| 873–993 `test_user_raster_submit` | real assembler plan-driven | `plan_submit.cpp` |
| 995–1046 `test_depth_oracle` | direct Reference oracle | `raster_oracles.cpp` |
| 34–153 Bytes4/Rgba/Extent2、unorm/requantize/exact_match/close_match/texel_offset/write_texel/fill_subresource/subresource_colour/texel_pattern/plain_view/full_target_quad/probe_module | 既有纯数据/oracle fixture | `raster_fixture.h/.cpp`（声明/定义分离） |
| main | 保持原顺序 | 原驱动；`raster_cases.h`声明 |

### Metal 19 mode与helper

| 原函数/行 | 类别 | 目标（tests/vertical_slice/metal/） |
|---|---|---|
| `run_task_tier0` 100–170 | plan-driven | `plan_compute.cpp` |
| `run_timeline` 172–263 | plan-driven | `plan_compute.cpp` |
| `run_access_certificate` 265–395 | plan-driven | `plan_compute.cpp` |
| `run_pointer_graph` 1367–1459 | plan-driven | `plan_compute.cpp` |
| `make_store_instruction` 910–920 | plan-driven | `plan_effect.cpp` |
| `make_store_pass` 922–930 | plan-driven | `plan_effect.cpp` |
| `store_word_pattern` 932–939 | plan-driven | `plan_effect.cpp` |
| `bytes_match_pattern` 946–951 | plan-driven | `plan_effect.cpp` |
| `run_effect_dag` 953–1365 | plan-driven | `plan_effect.cpp` |
| `run_tier1_indirect` 397–481 | direct-adapter | `direct_compute.cpp` |
| `run_cull_compact` 483–533 | direct-adapter | `direct_compute.cpp` |
| `run_cull_compact_1m` 535–592 | direct-adapter | `direct_compute.cpp` |
| `run_indexed_binding` 1461–1559 | direct-adapter | `direct_compute.cpp` |
| `run_pipeline_classification` 2750–2795 | direct-adapter | `direct_compute.cpp` |
| `run_representation_layer` 594–903 | direct-adapter | `direct_facets.cpp` |
| `run_sample_facet` 1718–1852 | direct-adapter | `direct_facets.cpp` |
| `run_checked_facet_generation` 1854–1979 | direct-adapter | `direct_facets.cpp` |
| `run_basic_raster` 1981–2108 | direct-adapter | `direct_raster.cpp` |
| `user_raster_msl_source` 1673–1716 | plan-driven | `plan_raster.cpp` |
| `run_task_graph_raster` 2110–2403 | plan-driven | `plan_raster.cpp` |
| `run_task_graph_raster_depth` 2405–2508 | plan-driven | `plan_raster.cpp` |
| `run_task_graph_raster_user_shader` 2510–2748 | plan-driven | `plan_raster.cpp` |
| `compile_and_submit_representation` 2820–2834 | plan-driven | `plan_representation.cpp` |
| `run_consume_input` 2836–3271 | plan-driven | `plan_representation.cpp` |
| `run_representation_churn` 3273–3443 | direct-adapter | `direct_representation.cpp` |
| `assemble_compute_plan` 27–39 | 共享既有fixture | `fixture.cpp` |
| `assemble_user_raster_plan` 41–46 | 共享既有fixture | `fixture.cpp` |
| `probe_task` 48–57 | 共享既有fixture | `fixture.cpp` |
| `make_probe_module` 59–78 | 共享既有fixture | `fixture.cpp` |
| `same_task` 80–98 | 共享既有fixture | `fixture.cpp` |
| `channels_close` 1566–1575 | 共享既有fixture | `fixture.cpp` |
| `fill_subresource` 1577–1592 | 共享既有fixture | `fixture.cpp` |
| `to_reference_coords` 1594–1601 | direct-adapter坐标类型转换 | `direct_facets.cpp` |
| `to_reference_vertices` 1603–1610 | 共享既有fixture | `fixture.cpp` |
| `to_reference_desc` 1612–1629 | 共享既有fixture | `fixture.cpp` |
| `complete_consume_proof` 1631–1638 | 共享既有fixture | `fixture.cpp` |
| `make_rgba8_view` 1645–1654 | 共享既有fixture | `fixture.cpp` |
| `make_depth32_view` 1656–1660 | 共享既有fixture | `fixture.cpp` |
| `metal_fullscreen_quad` 1662–1671 | 共享既有fixture | `fixture.cpp` |
| `make_epoch_probe_module` 2797–2818 | 共享既有fixture | `fixture.cpp` |
| StoreWord/WordAt | effect检查DTO | `plan_effect.cpp` |
| effect-dag 1250–1272 package/order篡改断言段 | semantic-boundary negative；同一Device/arena/compiled，不重置状态 | `semantic_negative.cpp::check_compiled_plan_tampering`；原位置调用 |
| Extent2/kNearestTol | 既有小DTO/常量 | `fixture.h` |
| ConsumeInput prepare_image lambda | 原数据准备函数体不变 | `fixture.cpp`；具名helper `prepare_consume_image` |
| ConsumeInput 2953–2965 post-consume sampling | 窄物理观察，保留同Device/arena/facet | `direct_representation.cpp::check_post_consume_sample` |
| ConsumeInput 3058–3132 fault-during块 | 既有explicit physical fault harness | `direct_representation.cpp::run_consume_fault_during`；原位置调用 |
| main十九mode | CLI原路由/条件/返回值保留 | 原驱动；`cases.h`声明 |

### Capture与构建

`tests/unit/capture_view_test.cpp` 仅CLI临时文件路径改为原子创建的独占目录，目录归本次
调用的局部RAII对象所有；只清理该目录。其余fixture函数/assert/CLI --write-fixture不变。
临时目录实现留本文件（不需要共享helper或新增runtime）。
`cmake/g5-tests.cmake` 显式 target_sources 四个旧测试target；根CMake只接入该fragment。
不改CTest名称、命令、属性、skip、不增加framework或G6数据驱动工具。

## 3. 实施结果与不变量

精确文件清单由上表对应到29个新增cpp和8个窄声明头；四个旧驱动只保留原调用/CLI路由，
另改capture测试与根CMake（相对本包基线仅新增一行fragment include），新增G5 fragment和本报告。
没有修改src/、公共ABI、schema、Vulkan测试、已有support harness或其他CMake fragment。
所有新增cpp在显式target_sources中仅登记一次；不创建测试框架、runtime或生产反向依赖。

- Core：7个职责TU；初始Arena仍活到main退出，共享ConsumeProof显式传参。
  原14块的顺序、块内allocation/facet/Device状态和全部assert保留。
  Reference提交场景的前置语义断言保持场景关联，不将伪造plan送入HAL。
- ExecutionPlan：validation、Node/effect、access、representation、lifetime及Reference submit分轨，
  原18个test函数的调用顺序不变；malformed sealing/preflight负例仍在该语义边界套件。
- Reference：sample/storage/attachment、raster/depth、FacetRef token oracle独立于真实assembler提交。
  原7块顺序、颜色公式、精度与负例不变。
- Metal：19个CLI mode仍是原CTest命令；plan_*只走真实assembler→compile→submit。
  独立direct_*复用G3非拥有harness；ConsumeInput后采样是同一Device/arena/facet的观察，
  不替代提交；fault-during仍是显式physical callback边界，不声称DeviceHal执行。
  effect-dag的package/order篡改抽成semantic_negative.cpp，按原调用点检查同一compiled/state。
- Capture：每次CLI调用通过create_directory原子认领唯一目录；发生碰撞重试，其他文件系统错误
  明确失败。不可复制RAII对象只删除自己成功创建的目录；不清理TMPDIR、不串行化CTest。
  原--write-fixture模式和capture字节/oracle全部不变。

## 4. 守恒与测试证据

`build/g5-baseline/check_conservation.py`逐函数比较token（保留字符串/字符字节，仅忽略注释与空白），
并比较21个Core/Reference原块、原main调用顺序、完整CTest映射。三个抽取helper在原调用点重新
展开后与原Metal函数体逐token相同；prepare_image的lambda体与新具名数据helper单独比较。
默认参数仅移到声明头，CHECK的失败机制不变。不是只比较函数名或assert数量。

| 门禁 | 结果 |
|---|---|
| 原函数体守恒 | 82个原函数（包括fixture）；Metal全部19 mode唯一归属，CLI body不变 |
| 原Core/Reference块 | 14+7=21块内容和调用顺序守恒 |
| 新头/函数owner | 8个头独立C++编译通过，无函数实现搬入头；ExecutionPlan/Metal旧函数均唯一owner |
| 断言表达式逐项多重集 | Core485；ExecutionPlan392个CHECK调用+1个macro定义；Reference212；capture105，全部相同 |
| Reference全量 | 初轮38/38，17.83秒；最终38/38，23.92秒；无skip，Debug+ASan/UBSan |
| 真实非沙箱Metal全量 | 初轮72/72，34.67秒；最终72/72，36.81秒；无skip，Debug+ASan/UBSan |
| 受影响22项各20次 | 两轮均440/440，73.43/72.66秒；3个Core/Reference套件+19个Metal mode，无skip |
| capture共享TMPDIR并发 | 两轮均8 worker共64进程，64/64；无遗留目录，同目录无关哨兵文件原样保留 |
| ON/OFF构建 | 两profile全部构建/最终链接通过；生产TU41/52；OFF均0项测试 |
| CTest映射 | Reference38/Metal72的name/command/properties与G5前逐项相同，摘要与G0相同 |
| OFF公共ABI smoke | Reference与真实Apple M1 probe+128×128 PPM成功；输出只落build |
| G1显式门禁 | 64类型、1409实现行守恒，16头自包含/无环 |
| G2显式门禁 | 24567字节与build/g2-reference/before.bin完全相同，未重录golden |
| 文档/静态 | docs.check、git diff --check通过；生产archive无vg::tests/AdapterHarness符号或反向include |

设备证据：Apple M1，macOS Version 26.6.2 (Build 25G83)，AppleClang21.0.0.21000101。
Reference/Metal原CTest映射SHA-256分别为
`0ccb72bbf83edbd11ee2ebe08c47926231cc71d6300bde312160f7c80a0ce63f`与
`2651d34acbf47db9be9940d3cf690f3f3fc24f995bc5f737f5335e0e34282ace`。
Compiler字节摘要为`dee036f482a910f9af0698817a241689c45a1d26f8321585796c4b7a659cffbd`。

额外复查G3生产搬迁仍109函数匹配、0问题；其旧baseline脚本最后强制monolith测试只能改receiver，
因此在本次已授权G5拆测试后按预期失败，未修改该历史脚本以隐藏此变化。测试守恒现在由本包
逐函数/块/原CLI检查承接。G4原64函数/19类型守恒仍通过，不把它算Linux SDK/GPU证据。

## 5. 可复现命令与产物

以下从仓库根执行。每次全量/重复执行先新建独占TMPDIR；Metal CTest和Metal OFF smoke在沙箱外。
不与其他工作包共用构建目录。

```sh
cmake -S . -B build/g5-reference -G Ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DVG_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake -S . -B build/g5-metal -G Ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DVG_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DVG_ENABLE_METAL=ON
cmake --build build/g5-reference -j4
cmake --build build/g5-metal -j4
g5_reference_tmp=$(mktemp -d /tmp/vg-g5-reference.XXXXXX)
TMPDIR="$g5_reference_tmp" ctest --test-dir build/g5-reference --output-on-failure
g5_metal_tmp=$(mktemp -d /tmp/vg-g5-metal.XXXXXX)
TMPDIR="$g5_metal_tmp" ctest --test-dir build/g5-metal --output-on-failure
g5_pressure_tmp=$(mktemp -d /tmp/vg-g5-pressure.XXXXXX)
TMPDIR="$g5_pressure_tmp" ctest --test-dir build/g5-metal -R '^(core.unit|core.execution-plan|reference.facet-oracles|vertical-slice.metal.(task-tier0|timeline|access-certificate|tier1-indirect|cull-compact|cull-compact-1m|effect-dag|pointer-graph|indexed-binding|representation-layer|sample-facet|checked-facet-generation|basic-raster|task-graph-raster|task-graph-raster-depth|task-graph-raster-user-shader|pipeline-classification|consume-input|representation-churn))$' --repeat until-fail:20 --output-on-failure
python3 build/g5-baseline/check_capture_concurrency.py
python3 build/g5-baseline/check_conservation.py
python3 build/g5-baseline/check_headers_and_owners.py

cmake -S . -B build/g5-off-reference -G Ninja -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug -DVG_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake -S . -B build/g5-off-metal -G Ninja -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug -DVG_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DVG_ENABLE_METAL=ON
cmake --build build/g5-off-reference -j4
cmake --build build/g5-off-metal -j4
python3 tests/tools/check_build_boundary.py build/g5-reference build/g5-off-reference
python3 tests/tools/check_build_boundary.py build/g5-metal build/g5-off-metal
build/g5-off-reference/vg-platform-probe --validate
build/g5-off-reference/vg-offscreen-triangle-ppm build/g5-off-reference/g5-smoke.ppm
build/g5-off-metal/vg-platform-probe --validate
build/g5-off-metal/vg-offscreen-triangle-ppm build/g5-off-metal/g5-smoke.ppm
python3 tests/tools/check_core_headers.py --generated-dir build/g5-reference/generated --baseline-ref 99ec414
python3 tests/tools/check_compiler_sources.py build/g5-reference --compare build/g2-reference/before.bin
ctest --test-dir build/g5-reference -R '^docs.check$' --output-on-failure
git diff --check
```

基线、机械搬迁/守恒工具及reference-final.log、metal-final.log、pressure-final.log
保存在本地ignored的build/g5-baseline；最终复跑使用--output-log显式保存，不依赖会被后续
CTest元数据查询覆盖的LastTest.log。不是提交或远程备份。
首次机械构建中的声明/type include遗漏已经修复，最终全量与表达式守恒通过；未通过改assert、
更换oracle、改skip或新增fallback获取绿灯。

## 6. 尚未关闭的门禁

G5按后端分批交付，此处只完成Core/Reference/真实Metal分轨。Vulkan测试未拆分，Linux SDK、
真实Vulkan与G4物理入口覆盖仍pending；未连接/配置远程环境，未声称整个跨平台G5或§9.3完成。
G6 runner/CMake数据驱动、vg-capture-view的OFF工具归属和既有链接重复warning未扩张处理。
没有新增public handle、submit入口、authority、资源/表示生命周期；ADR053/054和Stage0—7语义不变。
没有commit、push或切分支。实现、本机验证、M1平台验证、文档已交付；等待统筹独立复核。
