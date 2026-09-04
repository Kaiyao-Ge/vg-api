# G2：Compiler package、codegen 与独立 shader source 分离

日期：2026-09-03。Owner：G2 Compiler 子代理。状态：生产拆分、正式 Reference
验证与文档完成；统筹已完成源码登记和测试路径适配，独立审阅/真实 Metal/ON-OFF
由统筹汇总，Linux SDK/GPU 仍 pending。

## 冻结目标与边界

依据整改报告 §9.3/§10.1、原设计 02/03/05/10/11、G0 与
[G1/G2 并行冻结](g1-g2-orchestration.md)。本包仅移动内部编译器职责；不改变
shader 输出、package 字段、hash、source-map、binding、诊断、schema 或 ABI。
不新增运行时模板读取、生成器或框架；不修改 backend/Core/API/既有测试。
当前 main 的 G0/G1 改动保留；不切分支、不提交、不推送。中央 CMake 由统筹登记。

依赖方向为 package orchestration → 内部 codegen；独立 shader source 仅依赖
自己的常量和既有生成布局。compiler.h 保留兼容聚合入口，不要求 backend 同时迁移。
使用 codebase-memory 重新索引 `vg-api-g2-split`，查到 package 的 Reference/Vulkan/test
调用方；Objective-C++ 提取不完整，另用源码补证，未把缺失图边解释成无调用。

## 冻结清单迁移

所有生产相对路径均位于 src/compiler。

| 原 owner | 新 owner | 职责与分类 |
|---|---|---|
| compute_package.cpp 三个 build 函数 | compute_package.cpp | 生产 package 校验、稳定 allocation/binding 顺序、hash、source-map 与产物组织 |
| 三个 build 内的 MSL/GLSL 发射 | compute_codegen.h/.cpp | 内部纯 source 发射；只消费已验证 module 与 binding/table，不修改 package |
| task_ring_*_source | shaders/task_ring.cpp | 生产 compute publication source，继续消费 compute_task_ring.h 的 schema 片段 |
| sample_facet* 与 checked guard helper | shaders/facet.cpp | 物理 facet source；私有 guard 在同一翻译单元内复用 |
| raster_facet_*_source | shaders/raster.cpp | 物理 raster source，MSL 继续消费 vg_scene_root_msl.h |
| cull_compact_*_source | shaders/cull_compact.cpp | 窄物理实验 shader source，不变成 canonical IR 或新的生产 submit |
| compiler.h package declarations | compute_package.h | 原声明、结构布局与注释保持 |
| compiler.h source declarations/slot constants | shader_sources.h | 原 source 入口与 binding 常量保持 |
| compiler.h | compiler.h | 前端 CompileResult/compile_c_like + 兼容聚合 |

compute_task_ring.*、pipeline_classification.*、compiler.cpp、所有 schema/生成器、
backend 内嵌 source 均不修改。没有改写现有 test fixture 或 golden。

## 修改前基线

新增显式检查工具 tests/tools/check_compiler_sources.py，不登记新的 CTest。
先对 G0 已验证的 build/dev-reference archive 运行 driver，再移动生产代码。
初次配置 build/g2-reference 时 G1 正在迁移 core.cpp，中央源码清单尚未同步；
因此基线使用既有 G0 archive，未重配/重编 dev 目录。比较产物写入 build/g2-reference。

```sh
python3 tests/tools/check_compiler_sources.py build/dev-reference --record build/g2-reference/before.bin
```

基线：24,567 字节；SHA-256
`dee036f482a910f9af0698817a241689c45a1d26f8321585796c4b7a659cffbd`。
record 使用独占创建，拒绝覆盖旧基线。driver 输出每一个 package 字段，而不只比较 hash：
version/root_schema/canonical_ir_hash、完整 binding/table、source-map 的 index/line/source、
两端 source、ok/message，失败结果的默认 package 也纳入比较。

覆盖全部 10 个公开 source 函数：task_ring、cull_compact、sample_facet、
sample_facet_array、raster_facet 各 MSL/GLSL 两端。四个 sample 函数输出包含完整
generation guard/poison/function-constant 片段，私有 helper 不需要变成公开测试入口。

17 种 module 分别送入三个 package builder（51 个结果）：linear、atomic、pointer、
显式预计算 hash、仅 load_ref、publish、load size/alignment、atomic size/alignment、
load_ref size/alignment、via size/alignment、漏 effect、非法版本、漏 pointer edge。
正例包含非排序/重复 allocation、负值与大于一个字节的 store、load_via/store_via，
覆盖 sorted binding 与 first-seen indexed table 的区别。每例显式断言预期成功/拒绝，
不是只把未经检查的输出记录成真理。该有限矩阵不声称穷尽所有 IR 输入。

## 验收与四级状态

初步静态门禁已通过：package orchestration 内不再有 ostringstream、shader 源函数、
metal_stdlib/#version 或生成 layout macro；codegen 内没有 verify/hash/source-map 写入，
也不持有可写 package。四个新/兼容头分别通过 C++20 的自包含 syntax-only（含
`-Wall -Wextra -Werror`）。六个生产源也能直接编译链接并与原基线 cmp 完全一致。

```sh
for header in compiler/compiler.h compiler/compute_package.h compiler/compute_codegen.h compiler/shader_sources.h; do
  printf '#include "%s"\n' "$header" | c++ -std=c++20 -Isrc -Wall -Wextra -Werror -x c++ -fsyntax-only - || exit 1
done
c++ -std=c++20 -fsanitize=address,undefined -Isrc -Ibuild/dev-reference/generated build/dev-reference/g2-source-driver.cpp src/compiler/compiler.cpp src/compiler/compute_package.cpp src/compiler/compute_codegen.cpp src/compiler/shaders/task_ring.cpp src/compiler/shaders/facet.cpp src/compiler/shaders/raster.cpp src/compiler/shaders/cull_compact.cpp build/dev-reference/libvg_ir.a -o build/g2-reference/direct-source-driver
build/g2-reference/direct-source-driver | cmp build/g2-reference/before.bin -
```

额外全源 `-Werror` 检查沿既有 compute_task_ring.h → core.h 依赖发现
GraphEpochBuilder constructor 的 `-Wreorder-ctor`；未修改 Core 的现有初始化顺序。
这不影响上面的正常项目标志编译，已告知统筹，不把它混入 G2 修复。

中央 CMake 尚未登记两包新源时，另外直接编译执行三个现有测试（均 ASan/UBSan）：

```sh
c++ -std=c++20 -fsanitize=address,undefined -Isrc -Ibuild/dev-reference/generated tests/unit/compute_package_test.cpp src/compiler/compiler.cpp src/compiler/compute_package.cpp src/compiler/compute_codegen.cpp src/compiler/shaders/task_ring.cpp src/compiler/shaders/facet.cpp src/compiler/shaders/raster.cpp src/compiler/shaders/cull_compact.cpp build/dev-reference/libvg_ir.a -o build/g2-reference/compute-package-test
build/g2-reference/compute-package-test
c++ -std=c++20 -fsanitize=address,undefined -Isrc -Ibuild/dev-reference/generated tests/unit/compute_task_ring_test.cpp src/compiler/compute_task_ring.cpp -o build/g2-reference/compute-task-ring-test
build/g2-reference/compute-task-ring-test
c++ -std=c++20 -fsanitize=address,undefined -Isrc -Itests/support tests/fixtures/compute_package_golden_test.cpp src/compiler/compute_package.cpp src/compiler/compute_codegen.cpp build/dev-reference/libvg_ir.a -o build/g2-reference/compute-package-golden-test
build/g2-reference/compute-package-golden-test .
python3 tests/tools/test_schema_generator.py . build/g2-reference/schema-test
git diff --check
```

三个 C++ 测试退出 0，golden 原样通过；schema 测试退出 1，原因为下面的文件 owner
检查仍引用旧路径；git diff --check 通过。这不是正式 CTest 38 项完成证据。

### 唯一测试路径适配（统筹已完成）

冻结范围禁止 G2 修改既有测试，因此以下必要路径适配先上报，由统筹实施；
这里保留初次失败的原因和完整处置，不删除失败证据：

- tests/tools/test_schema_generator.py 第 41—42 行读取 compute_package.cpp 后检查
  `VG_SCHEMA_SCENEROOTRASTER_MSL_DECLARATIONS`。读取路径应改为 shaders/raster.cpp，
  原宏断言不变。
- 同文件第 80—81 行的 `schema::compute_task_ring::kShaderLayout` 出现两次与
  `word < 14u` 不存在的检查应另读 shaders/task_ring.cpp，原断言不变。
- 不把全部 compiler_source 引用简单指向同一文件；不在旧文件插入宏/字符串注释
  欺骗测试。没有其它 Python 测试硬编码 compute_package.cpp 路径。

中央 CMake 已增加 compute_codegen.cpp 与 shaders 下四个 .cpp；compute_package.cpp
继续保留。G1 新源同时登记后，正式执行以下命令：

```sh
cmake -S . -B build/g2-reference -G Ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DVG_ENABLE_SANITIZERS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/g2-reference -j4
python3 tests/tools/check_compiler_sources.py build/g2-reference --compare build/g2-reference/before.bin
ctest --test-dir build/g2-reference -R '^(compiler.compute-package|compiler.compute-task-ring|compiler.compute-package-golden|schema.generate)$' --output-on-failure
ctest --test-dir build/g2-reference --output-on-failure
```

正式结果（2026-09-03，build/g2-reference，Debug + ASan/UBSan）：

| 检查 | 结果 |
|---|---|
| 完整 CMake 配置/构建/链接 | 成功，129 个构建步骤；保留已有 duplicate-library linker warning，未扩张清理 |
| 同一 driver 对新 vg_compiler/vg_ir archive 比较 | 24,567 字节完全一致；SHA-256 与修改前相同 |
| package / ring / golden / schema 定向 CTest | 4/4，3.11 秒，无 skip |
| 完整 Reference CTest | 38/38，23.19 秒，无 skip；包括 docs.check 与 Vulkan source-contract |

测试总数未改变，新增检查工具没有登记 CTest。schema 的 owner 路径适配后原断言全部
通过，未以旧文件注释/假宏绕过断言。最终报告更新后 docs.check 1/1（0.10 秒），
git diff --check 通过。本目录全量的 capture.view 通过，未遇到并发临时文件冲突。

- 实现：生产职责搬迁完成；中央 source 登记、唯一测试文件的 owner 路径适配已由统筹完成。
- 本地验证：全输出字节比较、四头自包含、三个直接编译测试、静态门禁及正式 Reference
  38 项完成，相关四项定向回归完成。
- 平台验证：本子代理完成 Reference；真实 Metal 72 项及 ON/OFF 由统筹集成验证，
  不冒领其结果；Linux SDK/GPU 仍 pending。
- 文档：实现、有限基线、失败及关闭证据已记录。独立复核/clean integration 由统筹
  汇总，不以 GLSL source 比较代替 Linux SDK 编译或设备执行。
