# G0：构建基线与拆分边界

Owner：统筹代理（亲自实施，不委派）。日期：2026-09-03。
状态：实现与本机 Reference/真实 Metal 验证完成；Linux 平台验证仍 pending。

## 冻结范围

后续顺序更新（2026-09-03，用户明确要求）：G4不再等待Linux平台验证才开始源码拆分，
改为先实施、再在用户提供的远程机器完成SDK/GPU验收。下文G4启动依赖是原G0冻结快照；
当前安排见[G3/G4执行冻结](g3-g4-orchestration.md)。验证标准和pending状态不变。

- 依据整改报告 §9.3、§10.1 与 remediation-orchestration-memory。
- 起点：`main` / `99ec4148bfd0fdf91557c200b6bfb05397c1bed3`，与 origin/main 一致，工作树干净。
  此提交包含 MD-1、MD-2—6 实现和此前三项审查修复；不将已有成果重新实现。
- 唯一实现目标：生产库源码不依赖 BUILD_TESTING；Tier2 实验实现归测试支撑目标。
- 允许改动：`CMakeLists.txt`、`cmake/phase-d-d2.cmake`、`phase-d-d3.cmake`、
  `phase-d-d4.cmake`、`phase-d-d5.cmake`；新增独立构建边界检查工具
  `tests/tools/check_build_boundary.py`；本报告及必要的整改报告入口链接。
- 不改 C++/Objective-C++ 源码、公共 ABI、schema、capability、LoweringReport、Stage 0—7、
  mixed-domain narrowing、continuation/failure/hold/completion 顺序。
- 不拆 Core/Compiler/backend 文件，不迁移 facet harness，不重写 CMake/phase runner；这些属于 G1—G6。
- 不改变 CTest 名称、命令、属性、skip 条件或用例 oracle；预计 Reference 38、Metal 72。
- 本轮不自动提交或推送；保留 G0 独立 diff 供确认。

## 冻结清单与路径分类

| 当前来源 | 分类 | G0 处理 |
|---|---|---|
| phase-d-d2.cmake → discovery_stage.cpp | 公共生产提交 helper | 移到 vg_backend_reference 的无条件源码列表 |
| phase-d-d3.cmake → working_set_stage.cpp | 公共生产提交 helper | 同上 |
| phase-d-d5.cmake → envelope_stage.cpp | 公共生产提交 helper | 同上 |
| phase-d-d4.cmake → reference/tier2_oracle.cpp | Tier2 测试 oracle | 独立测试支撑库，只由对应测试链接 |
| phase-d-d4.cmake → metal/metal_tier2.mm | 窄物理实验 harness | 独立测试支撑库，只由对应测试链接 |
| 现有 assembler-driven / semantic-negative 测试 | 语义回归边界 | 不改用例与登记 |

静态核查：生产 HAL 仅保留 Tier2 header include，没有调用 Tier2 实验函数；遗留 include
清理留给后续后端拆分。本包不改变这些头文件或实验实现的物理位置。

## 验收计划

1. 修改前后比较完整 CTest name/command/properties 映射：38 / 72，必须完全一致。
2. Reference 与真实非沙箱 Metal 全量；dev presets 的 ASan/UBSan 保持开启。
3. 独立新构建目录中 BUILD_TESTING=OFF，Reference/Metal 全目标编译并完成可执行文件链接。
4. 构建边界检查：公共生产源在 ON/OFF 中各登记一次，Tier2 只在测试支撑目标编译，OFF 无 CTest。
5. 公共 ABI 示例与 probe smoke；无设备不得算真实 Metal 通过。
6. docs.check、Vulkan source-contract、git diff --check。
7. Linux SDK/Vulkan GPU 保持未验证；本机证据不替代 Linux 编译与真机执行。

## 结果与后续解锁

### 实施与红绿证据

- 修改前新建 `build/g0-off-reference`，关闭 BUILD_TESTING 后链接 `vg-platform-probe`
  失败；缺少 `run_discovery_stage`、`apply_working_set_budget`、`apply_envelope_continuation`。
  这确认了测试 fragment 持有生产源码登记的实际影响，不只是静态推断。
- 三个 helper 改为根 CMake 中无条件登记；D2/D3/D5 fragment 仅保留测试。
- D4 使用 `vg_tier2_oracle_harness` 与 `vg_metal_tier2_harness` 两个测试支撑库。
  两者不再向生产 backend archive 注入源码；Tier2 测试只链接所需支撑库及依赖。
- `check_build_boundary.py` 是显式运行的只读检查，不注册额外 CTest，不递归触发构建。
  检查 ON/OFF 生产源码清单、三 helper 唯一 owner、Tier2 owner、真实 archive 成员、OFF 零测试。
- 新工具也做了内存注入负例：遗漏 stage、重复 stage、harness 泄漏入生产目标均稳定拒绝。
- 图索引与源码交叉核查确认 Tier2 是测试调用；不因图工具未提取到 Objective-C++ 调用就认定无依赖。

### 验证命令与结果

以下均从仓库根运行；`dev-reference` / `dev-metal` 与两个 OFF 构建均开启 ASan/UBSan。

```sh
cmake --preset dev-reference
cmake --build --preset dev-reference -j4
ctest --preset dev-reference --output-on-failure
cmake --preset dev-metal
cmake --build --preset dev-metal -j4
ctest --preset dev-metal --output-on-failure
ctest --preset dev-metal -R '^(unit.tier2-oracle|vertical-slice.metal.tier2-nodes|api.mixed-domain.(reference|metal)|conformance.reference-mixed-domain|vertical-slice.metal.mixed-domain)$' --repeat until-fail:20 --output-on-failure

cmake -S . -B build/g0-off-reference -G Ninja -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug -DVG_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/g0-off-reference -j4
cmake -S . -B build/g0-off-metal -G Ninja -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug -DVG_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DVG_ENABLE_METAL=ON
cmake --build build/g0-off-metal -j4
python3 tests/tools/check_build_boundary.py build/dev-reference build/g0-off-reference
python3 tests/tools/check_build_boundary.py build/dev-metal build/g0-off-metal

build/g0-off-reference/vg-platform-probe --validate
build/g0-off-reference/vg-offscreen-triangle-ppm build/g0-off-reference/g0-smoke.ppm
build/g0-off-metal/vg-platform-probe --validate
build/g0-off-metal/vg-offscreen-triangle-ppm build/g0-off-metal/g0-smoke.ppm
python3 tests/vertical_slice/vulkan_capability_contract_test.py .
git diff --check
```

Metal CTest 与 OFF smoke 在非沙箱执行；probe 实际枚举 Apple M1，不把 Reference 回退算作 Metal。
新 OFF 目录不是从 ON 目录切换，避免残留 CTest/对象掩盖依赖缺失。

| 检查 | 实际结果 |
|---|---|
| Reference 全量 | 38/38，19.16 秒，无 skip |
| 真实 Metal 全量 | 72/72，35.01 秒，无 skip |
| Tier2 + Mixed-domain 重复 | 六项各 20 次，120/120，20.91 秒，无 skip |
| Reference OFF 全目标 | 构建及最终可执行文件链接通过；生产源码 27 个编译单元，CTest 0 |
| Metal OFF 全目标 | 构建及最终可执行文件链接通过；生产源码 29 个编译单元，CTest 0 |
| ON/OFF 边界工具 | 两个 profile 均通过；生产 archive 不含 Tier2 |
| OFF probe / 公共 C ABI raster smoke | Reference、真实 Metal 均成功；128×128 PPM 只写 build 目录 |
| Vulkan source-contract | 通过；不是 Linux SDK 编译或 GPU 执行 |
| 文档与静态门禁 | docs.check 1/1；git diff --check 通过；src/include/schemas 与既有测试用例相对起点零 diff |

修改前后 `ctest --preset <preset> --show-only=json-v1` 的完整
`name` / `command` / `properties` 数组逐项完全相同（排除非合同的 CMake backtrace）。
将 build 路径替换为 `<BUILD>`、仓库路径替换为 `<ROOT>` 后，以工具规定的 JSON
规范化方式得到冻结摘要：

| Profile | CTest 映射 SHA-256 |
|---|---|
| dev-reference | `0ccb72bbf83edbd11ee2ebe08c47926231cc71d6300bde312160f7c80a0ce63f` |
| dev-metal | `2651d34acbf47db9be9940d3cf690f3f3fc24f995bc5f737f5335e0e34282ace` |

摘要冻结的是 G0 基线，不要求后续经批准的测试拆分永远维持同一命令；G5/G6 必须给出逐项迁移表。

### 发现但不扩张的事项

- `cmake/phase-d-d6.cmake` 中 `vg-capture-view` CLI 仍只在 BUILD_TESTING=ON 时提供。
  它不是被 production library 引用的缺失实现；工具产品化归属留给 G6。
  检查工具显式冻结这一个差异，新增或消失的条件工具会报错，不用宽泛排除掩盖问题。
- `vg-golden-gen` 仍引用 tests/support 中的格式 helper；不在 G0 迁移。
- Metal 对 Reference 的 HostAssisted 依赖、公共 helper 当前由 vg_backend_reference 承载、
  旧 Tier2 include、已有重复链接库 warning 均不在本包重新分层。
- Vulkan source-contract 对单一源码文件布局的依赖由 G4 随源码搬迁一起修订，不能删除保护。
- Linux SDK、glslc 和真实 Vulkan 设备尚无本轮执行证据。需要在 Linux 先建立
  `cmake --preset dev-vulkan` → build → `ctest --preset dev-vulkan` 基线，再建立相同选项的
  `BUILD_TESTING=OFF, VG_ENABLE_VULKAN=ON` 新目录并运行边界检查。Linux 测试数量未核证，不能杜撰。

### 后续工作包边界冻结

| 包 | 可以改什么 | 不可以顺手做什么 | 启动依赖 |
|---|---|---|---|
| G1 Core | core.h/.cpp 按 Arena、Facet/Representation、Graph、Effect/Access、Node、Envelope、Result 迁移；聚合头兼容；对应 include/source 登记 | 改生命周期、authority、Task ABI、assembler/schedule 算法；重造已独立模块 | G0 本机基线；保留聚合头供并行包使用 |
| G2 Compiler | compute_package/compiler.h 中 package 与 shader/codegen 分离，继续消费现有 schema | 改 shader 行为、binding/hash/layout；让运行时依赖外部 shader 文件 | G0；保留调用接口，不碰 backend 主文件 |
| G3 Metal | 私有状态 → resource/cache → pipeline/lowering → encoding/commit；窄物理 harness | 改 Stage 顺序、Mixed-domain 语义、缓存寿命、报告计数事实；新增公共虚接口 | G0；每阶段真实 Metal 验证 |
| G4 Vulkan | 与 G3 对应的 Vulkan 私有职责拆分，迁移静态检查到新 owner | 放开 Raster capability；用 stub 编译/静态检查代替 SDK 与 GPU | G0 + Linux 构建/设备基线 |
| G5 测试 | 明确 plan-driven、direct-adapter、semantic-negative 三类归属，搬迁大测试入口 | 通过 harness 替代 assembler；放宽 oracle/负例/skip | 对应后端 harness 接口完成；Core 测试拆分等 G1 |
| G6 工具 | 显式 source/schema/test 表、统一登记函数、数据驱动 phase runner | 顺手改实验 gate/schema/结果分类；把缺测、skip 或 review-only 写成通过 | 各包文件/测试清单稳定 |

G1/G2 可并行；G3 可在接口兼容前提下加入。中央 CMake 只能有一个集成 owner。
G5 按后端分批解锁，不必等待另一个后端；G6 在清单稳定后收口。
本轮仅解锁后续规划/实施条件，不自动开始任何 G1—G6 工作。

四级状态：实现完成、本机验证完成、Reference/真实 Metal 平台验证完成、文档已记录；
Linux 平台验证仍 pending。自审与实测由统筹代理执行，不称为另一代理的独立审查。
下一包开始前需将本包经确认的变更形成独立集成检查点；本轮未提交/推送。
