# G6 构建登记与实验 runner 治理

2026-09-03；统筹亲自实施，不启动子代理。起点main99ec414加未提交G0—G5；全部保留。
依据整改报告§9.3/10.1、G0工作包表。当前Core/Reference/Metal清单稳定，G5已审查通过。
Vulkan源文件清单已有G4实施，本轮只迁移现有构建登记，不拆Vulkan测试或开放capability；
Linux SDK/真实Vulkan仍pending，不能以本机验证宣布跨平台验收。

## 冻结范围与顺序

1. CMakeLists.txt、cmake/phase-d-d2—d7.cmake、phase-e-e1/e4/e6.cmake：重复target/测试登记
   转为声明式行与统一helper；phase fragment显式列举，缺文件应报错，不再glob静默收集。
   新增cmake/registration.cmake、schemas.cmake、targets.cmake、tests.cmake。
   G3/G4/G5的显式source fragment继续唯一拥有其源清单，不复制另一个manifest。
2. 三个现有schema生成规则用同一helper登记，保留输出名/生成器参数/依赖/字节。
   vg-capture-view移入常规工具登记，使OFF也可用；test fixture生成仍仅ON。
3. tools/vg-exp/vg_exp.py：Phase A—E使用唯一load/run/summary/report循环；
   新增tools/vg-exp/phase_catalog.json声明定义文件、ctest/backend、gate及非执行evidence。
   保留CLI子命令、实验顺序、E004 B/D区别、E013非gate、D分类、E政策、benchmark P0边界。
4. tests/tools/check_build_boundary.py只收紧OFF工具一致性，删除capture-view唯一例外。
   新增tests/tools/test_phase_runner_contract.py及tests/fixtures/phase-runner-contract.json，
   检查旧成功/失败产物守恒和missing/skip不得passed；登记一个独立非GPU工具测试。
   新增tests/tools/check_g6_build.py显式检查原target/source/CTest/schema守恒及增量生成。

不动src/、public ABI、schema内容、shader、backend执行/生命周期、既有实验definition/gate。
旧phase runner A—C将无测试的exit0当passed，所有旧phase均未识别skip；本轮统一为missing/
skipped且summary失败，修复不能假绿的工具边界，不提高review-only等级或编造测量。
正常执行仍保持旧sample/report/schema；新的缺测/skip反例单列，不重录golden掩盖差异。

## DoD与证据计划

基线保存build/g6-baseline：原CMake/runner、source/target/CTest映射、生成物字节。
保持原Reference38/Metal72名称/参数/属性，新增一个contract测试后预期39/73；
ON/OFF生产41/52单元不变，OFF测试0，工具清单完全一致。全部显式登记，无生产依赖tests。
Reference/真实非沙箱Metal全量，工具负例和Phase A—E/benchmark回归，OFF完整链接/probe/
capture-view smoke，G1/G2/G4/G5静态守恒、schema删除单个输出后重生/no-op构建均需通过。
复核由本人另作一轮源码/契约检查，按用户要求不称为另一代理独立审查。
不commit/push/切分支，不启动G7或远程环境；完成后记录准确结果与残余平台缺口。

## 实施结果与迁移归属

| 旧职责 | 当前唯一归属 | 处置 |
| --- | --- | --- |
| 顶层构建初始化、选项、sanitizer | `CMakeLists.txt` | 只保留初始化和显式include；不改变选项默认值 |
| 重复target/CTest/schema操作 | `cmake/registration.cmake` | 统一登记函数；不隐式提高C++标准，不发现源码；重复测试/错误target声明报错 |
| 三个schema及13个产物 | `cmake/schemas.cmake` | 显式输入/输出；保留生成器和依赖 |
| 生产库、产品工具、平台条件 | `cmake/targets.cmake` | 显式source/link/include/feature/property；继续使用G3/G4源码清单 |
| 原根目录测试与mode登记 | `cmake/tests.cmake` | 原顺序/命令/属性；19个Metal mode、4个Vulkan mode改为列表 |
| D2—D7、E1/E4/E6测试 | 原9个`cmake/phase-*.cmake` | helper登记；由tests.cmake按原次序显式include，无glob |
| capture-view工具与fixture | `targets.cmake` / `phase-d-d6.cmake` | CLI与测试解耦；fixture仍只在ON生成 |
| Phase A—E配置 | `tools/vg-exp/phase_catalog.json` | 5组显式定义文件、CTest/backend、gate、非执行证据、输出字段和报告文字 |
| Phase A—E执行 | `tools/vg-exp/vg_exp.py` | 唯一load/execute/summary/report路径；无旧阶段别名或重复循环 |
| 工具与迁移验证 | `check_build_boundary.py`、`check_g6_build.py`、`test_phase_runner_contract.py` | OFF工具一致性；构建守恒；运行证据契约 |
| 固定旧输出基线 | `tests/fixtures/phase-runner-contract.json` | 从未改动的G6前runner生成10组hash/调用清单；未用新实现重录 |

`vg_exp.py`由1044行变为430行，另有520行声明数据；顶层CMake由537行变为25行，
其登记内容仍显式保存在分层文件中。行数不是验收条件，未以换文件位置代替统一执行逻辑。
既有probe/run/analyze及benchmark接口保留；benchmark仍为host-clock P0，不产生GPU计时主张。

### 允许的行为差量

1. OFF现在也生成`vg-capture-view`，没有增加测试/fixture或生产库到测试库的依赖。
2. Phase CTest过滤由未转义点号改为literal name，避免误匹配其他测试。
3. Phase A—C无测试但exit0不再passed；A—E及共用状态函数识别skip/not-run，summary失败。
   review-only始终是非执行证据。成功与真实失败的旧JSON/CSV/Markdown/manifest字节保持一致。
   CLI仍按既有约定返回产物路径，证据状态以summary/sample为准，未顺手改退出码协议。
4. 只新增`tooling.phase-runner-contract`一个非GPU CTest。原38/72条映射全部保留，故总数39/73。

## 实测验收（2026-09-03）

环境：Apple M1、AppleClang 21、Ninja、Debug、ASan+UBSan。真实Metal在非沙箱运行；
四棵独立构建树为`build/g6-{reference,metal,off-reference,off-metal}`。

| 检查 | 结果 |
| --- | --- |
| Reference全量 | 39/39，无失败/skip；最终一轮9.71秒 |
| 真实Metal全量，含Phase A—E/benchmark | 73/73，无失败/skip；最终一轮16.68秒 |
| 旧target/source归属 | 两profile全部相同；编译命令参数按token对比无增删，保留C++17 ABI测试 |
| 原CTest名称/顺序/命令/属性 | Reference38、Metal72逐项相同，唯一增量是contract测试 |
| ON/OFF生产边界 | 41/52生产编译单元不变；OFF均0测试、无test源码/Tier2，工具清单完全一致 |
| OFF产品smoke | 两profile的probe、公共ABI PPM、capture-view均成功；Metal probe观察到Apple M1 |
| schema | 两profile各13产物逐字节相同；每个schema分别移走一个输出后重生共6例通过；no-op不改mtime |
| runner契约 | 10组旧成功/失败产物及路由守恒；15组missing/skip/not-run反例；4组错误配置反例 |
| G1守恒 | 64类型、1409实现行，16自包含头、无include环/逆依赖 |
| G2守恒 | 24567字节相同；SHA-256 `dee036f482a910f9af0698817a241689c45a1d26f8321585796c4b7a659cffbd` |
| G3生产/G4源码守恒 | Metal109个函数体；Vulkan64函数/19类型；Vulkan此项只是源码检查 |
| G5守恒 | 82函数、21有序块、19Metal mode、原assert/CHECK、8自包含头通过 |
| 文档/静态 | docs.check、git diff --check通过；旧阶段runner函数/调用在tools/tests中零命中；CMake无GLOB |

PPM smoke只证明现有公共ABI工具工作，不另当作新的Metal raster或Vulkan证据。
G3旧脚本末尾的“仅receiver变化”条件已由G5测试拆分替代，本轮只执行其生产函数守恒部分，
测试守恒交由G5的82函数/21块核查，未修改历史脚本或把其全部宣称为通过。

新增CTest后的完整映射摘要（去除build/root路径差异）：

- Reference：`034c980b6b8c59f1aa204f31c538764bff633ae171e6a1a2cc55728b060e2bad`
- Metal：`7ea859aae583ac0f36b778622d47704aa530693f3d86da756bd8f0e7911e3c28`

### 复现命令与证据位置

初始四次configure统一使用`-G Ninja -DCMAKE_BUILD_TYPE=Debug -DVG_ENABLE_SANITIZERS=ON
-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`，分别设置`BUILD_TESTING=ON/OFF`与`VG_ENABLE_METAL=ON/OFF`，
随后`cmake --build BUILD_DIR -j4`。全量日志在`build/g6-baseline/reference-final.log`与
`metal-final.log`；旧基线/命令hash在同目录，属于本地忽略产物，不是已提交远程备份。

```sh
ctest --test-dir build/g6-reference --output-on-failure -j4
# 以下Metal命令必须在真实设备可见的环境执行：
ctest --test-dir build/g6-metal --output-on-failure -j4
python3 tests/tools/check_build_boundary.py build/g6-reference build/g6-off-reference
python3 tests/tools/check_build_boundary.py build/g6-metal build/g6-off-metal
# --incremental会暂时搬走指定生成文件，只能在空闲构建树执行：
python3 tests/tools/check_g6_build.py build/g6-baseline/state.json build/g6-reference dev-reference --incremental
python3 tests/tools/check_g6_build.py build/g6-baseline/state.json build/g6-metal dev-metal --incremental
python3 tests/tools/test_phase_runner_contract.py
python3 tests/tools/check_core_headers.py --generated-dir build/g6-reference/generated --baseline-ref 99ec414
python3 tests/tools/check_compiler_sources.py build/g6-reference --compare build/g2-reference/before.bin
python3 build/g4-baseline/check_conservation.py
python3 build/g5-baseline/check_conservation.py
python3 build/g5-baseline/check_headers_and_owners.py
python3 tools/vg-docs/vg_docs.py .
git diff --check
```

## 本人复核与剩余边界

按冻结DoD另作源码复核：生产链接方向、语言标准/宏、生成依赖、CTest注册条件、实验顺序、
E004历史定义区分、E013非gate、D classification、E证据政策和manifest均保持；未发现本包阻断项。
只修正搬迁后的过时CMake注释并统一剩余Python测试登记，未扩大到backend/ABI/实验定义。
按用户要求没有启动子代理；本记录是本人实施后的复核，不冒充另一代理的独立审查。

§10.1架构演练：未新增公共对象、生命周期、submit或authority；没有改变TaskGraph/NodeRef。
未来ray/neural仍扩展现有contract/schema/capability/backend lowering，本包没有引入顶层领域字段。

| 层级 | 状态 |
| --- | --- |
| implemented | G6冻结清单全部实施 |
| locally verified | Reference/工具/静态守恒完成 |
| platform verified | 真实Metal完成；Linux SDK/真实Vulkan pending，不用macOS静态检查替代 |
| documented | 本报告与总整改报告§9.3已更新 |

Linux后续应在真实Vulkan机器执行ON/OFF configure/build、CTest及同一边界检查，并记录
capability/Unsupported和硬件结果；本轮没有连接远程、安装SDK或扩大G4/G5验证声明。
G6在本机可验收范围闭合，**不是全部跨平台整改完成**。全部变更仍留在main工作树，未提交/push。
