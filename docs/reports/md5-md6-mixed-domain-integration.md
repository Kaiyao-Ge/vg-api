# MD-5/6 mixed-domain 实施与验收台账

日期：2026-09-03。依据：整改报告 §9.2.13、[ADR-054](../decisions/ADR-054-mixed-domain-execution-schedule.md)。
本记录描述共享 `main` 工作树，保留 MD-2/3/4 累积修改，不创建提交或推送。

## 冻结范围与依赖

MD-1 sealed schedule → MD-2 Stage 6/7 physical transition contract → MD-3 Reference
语义基线 → MD-4 Metal；MD-5 Vulkan 与 MD-6 公共路径/conformance/docs 可并行，
但 MD-6 最终跨后端关门依赖 MD-5 的 Linux SDK/真机门禁。

- MD-5 只迁移 Vulkan 生产调度和对应 vertical slices；不开放 Raster capability。
- MD-6 只补公共路径验收、测试注册和实施状态文档；不新增 API，亦不重新实现
  Core schedule、lifetime、representation 或后端算法。
- `VgTaskRecordV2`、ABI 版本、ring schema、资源生命周期、SceneRoot narrowing、
  同 NodeRef 跨域拒绝均冻结；§9.3 治理与原生并行优化不在本包。
- Metal canonical mixed 采用保守串行。R→C 64-bit atomic 的 HostAssisted 证据不
  代表原生 render→compute fence；restricted user-raster mixed 仍整体拒绝。

## 公共路径证据与文件清单

| 文件 | 归属与可观察验收 |
|---|---|
| `tests/api/vg_mixed_domain_conformance_test.cpp` | 新公共 ABI-only 测试；只包含 `vg/vg.h`、链接 `vg_api`；实际 backend 独立选择，不回退冒充 Metal/Vulkan |
| `tests/api/vg_c_abi_conformance_test.cpp` | 旧 restricted NodeRef 两域复用负例改用合法 root，断言 Node domain mismatch；不再冒充合法 mixed 整计划拒绝证据 |
| `CMakeLists.txt` | 新增 `api.mixed-domain.reference`，Metal/Vulkan preset 各另加本后端目标；Reference 总数 +1，Metal/Vulkan 总数 +2 |
| `docs/START.md`、`docs/decisions/ADR-054-mixed-domain-execution-schedule.md`、整改报告、本台账 | 只更新实施/证据状态，不改冻结决定 |

新 C ABI fixture 使用两份真实 CodeObject、两个合法完整 NodeRef、同一个 TaskGraph、
授权 Envelope。以反存储顺序依赖验证 C→R（Compute 写 source，Raster 输出非零
像素），以 raster-only oracle 后接 atomic 验证 R→C 输出严格相等；再次提交新
公共 graph 检验结果稳定和 hold 已释放。相同 sealed graph 的 repeat-submit 则由
既有 MD-3/4 测试负责。Metal 的 R→C report 明确检查 `metal_pipeline` 的
HostAssisted；不同 NodeRef 的 restricted mixed 与 Vulkan Raster mixed 必须返回
`VG_ERROR_UNSUPPORTED`、空 Submission、source/target 字节不变。

只读核证 `src/api/vg_api_execution.cpp`：公共 submit 仍唯一调用
`ExecutionPlanAssembler::assemble` → `DeviceHal::compile` → `submit`；编译拒绝在
Commit 前。公共 ABI 没有 published Task 列表或 per-Node package 结构查询口，
因此不为测试扩张 ABI；精确 identities、完整 canonical/Envelope publication、
failure reachability、representation 一次执行、hold 释放与 transition admission
复用以下真实 assembler 驱动证据：

- `tests/unit/execution_schedule_test.cpp`、`device_hal_transition_contract_test.cpp`；
- `tests/conformance/reference_mixed_domain_conformance.cpp`；
- `tests/vertical_slice/metal_mixed_domain_conformance.cpp`；
- `tests/unit/envelope_continuation_test.cpp` 与后端 continuation slices；
- MD-5 的 `tests/vertical_slice/vulkan_task_timeline_test.cpp`。

没有新增 malformed/stamped-plan fixture 或把 narrow physical harness 当公共执行。

## 初次交付四级状态与平台门禁（历史证据）

| 工作包 | 已实施 | 本地验证 | 平台验证 | 文档 |
|---|---|---|---|---|
| MD-4 前置 | 完成 | 前置 Reference 37/37 | 前置真实 Metal 70/70 + 80 次重复 | [原交付记录](md4-metal-mixed-domain-completion.md)，不回溯改写 |
| MD-5 Vulkan | 生产与专属测试已实施，待独立复核 | macOS source contract 通过；不等于 Linux 编译 | **Linux SDK/真机 pending** | 本台账记录缺口 |
| MD-6 公共验收 | 已新增测试与文档，受下述错误分类缺口阻塞 | Reference **38/38**（19.60 秒） | 非沙箱真实 Metal **71/72**（33.07 秒），唯一失败为下述错误码；Vulkan **pending** | 实施状态与阻塞已记录 |

### 初次交付阻塞：Metal restricted mixed 的公共错误码

真实 Metal 的新公共测试已通过 C→R 与 R→C 输出检查，但合法 distinct-NodeRef
restricted mixed 的 Stage 6 拒绝诊断没有 `Unsupported` 标识：
`Metal restricted user raster shaders cannot participate in a native mixed-domain ExecutionSchedule`。
`src/api/vg_api_execution.cpp::classify_plan_error` 因此返回
`VG_ERROR_INVALID_ARGUMENT` 而不是要求的 `VG_ERROR_UNSUPPORTED`。
这不是开放 restricted shading 的要求，只需使该明确 Unsupported 拒绝的诊断与
公共分类一致。已向统筹代理报告；MD-6 测试/文档 owner 没有越权修改 Metal 生产
文件，也没有放宽测试接受错误码。修复后必须重新跑本节真机验收再关闭该阻塞。

不能用 macOS Python source contract 或 Metal 全绿替代 Vulkan 编译/执行。当前没有
可用 Linux runner/SDK/device 通道；需用户提供入口后运行 Linux preset。历史阶段
compile-review-only 先例不会自动豁免本轮明确要求的 Linux 平台门禁。

## 验证命令

```sh
cmake --build --preset dev-reference -j4
ctest --preset dev-reference --output-on-failure
cmake --build --preset dev-metal -j4
ctest --preset dev-metal --output-on-failure
ctest --preset dev-metal -R '^api.mixed-domain' --repeat until-fail:20 --output-on-failure
git diff --check
```

真实 Metal 必须在可见 MTLDevice 的非沙箱环境运行；skip 77 不计作平台通过。
Linux 需在真实 runner 中先 configure/build `dev-vulkan`，再运行该 preset 全量，
保留 Vulkan device/driver、validation 与原始测试日志。

联合静态搜索在 MD-5 调度修改后零命中：三个 backend 生产路径没有 legacy
`plan.task_order`、`validated_effect_graph`、`effect_graph_deterministic_order`、
`classify_effect_graph_shape`、`resolved_nodes[0]/front()` 或 package-first 投影。
Metal production ring 的 compute-only guard 在 pack 前执行，mixed 使用 host
publication；Tier1 ring 属显式窄物理 harness，typed codec 仍拒绝 Raster。Vulkan
Raster 在 Stage 6 整体拒绝，不能到达 ring。公共 ABI/layout 和生成 schema gate 随
全量执行，不因为新测试而改变 oracle 或降低阈值。

## 有界审查修复（2026-09-03）

本次仅修复审查指出的两个代码缺口，不覆盖或回写上面的初次失败证据：

| 文件 | 本次增量与分类 |
|---|---|
| `src/backends/vulkan/vulkan_device_hal.cpp` | 生产路径：唯一一次共享 `apply_envelope_continuation` 移至 Timeline 只读 precheck 后、lifetime prepare/representation commit/GPU dispatch/输出回写前；拒绝直接返回 `false`，不制造已执行的 `PartiallyProduced`；后续 ring 复用已确定的 publication order |
| `src/backends/metal/metal_device_hal.mm` | 生产路径：restricted user-raster mixed 诊断增加 `Metal Unsupported:`，使既有公共分类返回 `VG_ERROR_UNSUPPORTED`；不开放该路径，不修改分类体系 |
| `tests/vertical_slice/vulkan_task_timeline_test.cpp` | 真实 assembler 驱动 submit 边界：Rejected、unknown token、consumed token、noncanonical suffix 四类拒绝。后者由另一张真实反向依赖图提交产生合法 token，不注入 frozen plan。反例包含可观察 store、独立 allocation 的合法 representation request、合法 Timeline signal，检查字节/epoch/facet generations/holds 不变及执行计数为零；后续成功提交同一 signal 验证拒绝未推进 Timeline。quota/resume 检查完整三 Task 均执行、publication 分别为 canonical prefix/suffix |
| `tests/vertical_slice/vulkan_capability_contract_test.py` | 静态补证：共享 continuation 调用恰好一次且先于 lifetime/representation/dispatch/writeback/ring；不是 Vulkan 平台执行证据 |
| 本台账 | 保留历史失败，追加修复后结果与 Linux pending |

未修改 Core/shared envelope helper、API 生产分类、公共 API 测试断言、ABI、CMake、
ring schema 或其它 backend 生命周期顺序；没有新增窄 physical harness。

### 修复后验证

| 命令 | 实测结果 |
|---|---|
| `cmake --build --preset dev-reference -j4` | 通过 |
| `ctest --preset dev-reference --output-on-failure` | **38/38**，14.22 秒 |
| `cmake --build --preset dev-metal -j4` | 通过 |
| 非沙箱 `ctest --preset dev-metal --output-on-failure` | 真实 Metal **72/72**，29.70 秒；无 skip，原公共 mixed 错误码失败已转绿 |
| 非沙箱 `ctest --preset dev-metal -R '^api.mixed-domain' --repeat until-fail:20 --output-on-failure` | 两个 case 各 20 次，共 **40/40**，5.46 秒；包括真实 Metal |
| `clang++ -std=c++20 -fsyntax-only -Isrc -Iinclude tests/vertical_slice/vulkan_task_timeline_test.cpp` | 通过；只是测试源语法检查 |
| `clang++ -std=c++20 -fsyntax-only -Isrc -Iinclude -Ibuild/dev-reference/generated src/backends/vulkan/vulkan_device_hal.cpp` | 通过；只是 macOS **非 `VG_HAS_VULKAN` stub** 语法检查，不是 Vulkan SDK 编译 |
| `python3 tests/vertical_slice/vulkan_capability_contract_test.py .`、`git diff --check` | 通过 |

初次 stub 语法检查遗漏 generated include 路径而失败，以上补齐 include 后通过；
不将这次本地命令修正误记成 SDK/设备故障。Vulkan 生产路径静态搜索仍无
`plan.task_order`、`validated_effect_graph`、`sealed_structural_barriers`、
`effect_graph_deterministic_order`、first/global Node projection。

当前四级状态：两处代码修复已实施，Reference/Metal 本地与真实 Metal 门禁通过，
台账已更新，等待独立复核。**Linux SDK/真实 Vulkan 全量仍 pending；新增 Vulkan
拒绝与恢复反例尚未在 Vulkan 设备运行。** 本轮没有 Linux 入口，没有运行 Linux，
不能据此宣布 MD-5/6 或 §9.2.13 整路线完成。

### 独立复核与测试预期修正（2026-09-03）

统筹代理独立复跑 Reference **38/38**、非沙箱真实 Metal **72/72**；两处生产
修复通过源码复核。但 Vulkan 恢复测试在缓冲区填充 `0x55` 后只写前 4 字节，
却用 8 字节读取期待高位为零。现已修正两处断言：比较完整 allocation，
前 4 字节必须为 `0x07`，其余字节必须保持 `0x55`，不清零或忽略未写区域。
本次仅修改该测试及台账，没有改动生产代码。

修正后测试源 `clang++ -std=c++20 -fsyntax-only -Isrc -Iinclude
tests/vertical_slice/vulkan_task_timeline_test.cpp`、Vulkan Python source contract
及 `git diff --check` 通过；Reference preset 中 `compiler.compute-package`、
`core.envelope-continuation`、`vertical-slice.vulkan.capability-contract` 定向回归
**3/3** 通过。这些本地验证不包含 Vulkan GPU 执行，**Linux SDK/真机验证仍 pending**。

## Mixed-domain 设计审查三项修复（2026-09-03）

依据 ADR-054 Decision 1/5/6/7，范围限定为以下三项，不重新开放 restricted-MSL
mixed、SceneRoot、Vulkan Raster，不改 ABI、Core schedule、HAL 公共结构或 ring。

| 审查缺口 | 修复及文件 |
|---|---|
| Reference/Metal continuation 拒绝晚于 representation commit | `src/backends/reference/reference_device_hal.cpp`、`src/backends/metal/metal_device_hal.mm`：共享 continuation helper 只调用一次，先于 lifetime prepare/representation commit，发布复用已确定 order。保留 Timeline（Metal 另含 sealed-effect）校验先于 token 消费，失败无 representation/执行副作用 |
| Reference 越过 restricted-MSL mixed narrowing | Reference Stage 6 在 per-Node package 构建前整计划 Unsupported，诊断含完整 Raster NodeRef/domain；`tests/api/vg_mixed_domain_conformance_test.cpp` 同时验 Reference/Metal 拒绝及不执行支持子集，另验证 Reference restricted raster-only 仍成功 |
| Reference 把计划 schedule/transition 成本当成执行实绩 | Reference submit 初始化输出，清除计划 `schedule_program_order` 事件和 transition fallback 计数；按实际进入的 Task（含失败尝试，不含取消）及实际进入的 consumer wave 记账，保留 CompiledPlan 的计划成本与 per-Node 编译历史 |

测试清单与分类：

- `tests/support/mixed_continuation_contract.h`：共享的真实 assembler conformance，
  不是 direct-adapter harness；在独立 allocation 上提供合法 representation request
  和旧 FacetRef，检查 Rejected、unknown/consumed token、非 canonical suffix、
  Timeline wait 拒绝的 bytes/epoch/facet generations/holds/物理计数不变；
  用真实反向图提交生成不匹配 suffix，成功恢复沿用同一 Timeline signal，且证明
  quota 只筛选 publication、不裁剪执行。主机 sentinel 改写遵守 content epoch 通知。
- `tests/support/assembled_plan_fixture.h`：仅补 multi-Node fixture 遗漏的
  `pending_overflow` → assembler 输入传递，没有手写 frozen plan。
- `tests/conformance/reference_mixed_domain_conformance.cpp`、
  `tests/vertical_slice/metal_mixed_domain_conformance.cpp`：接入共享矩阵；Reference
  另覆盖成功/失败尝试/取消后继/独立分支/Timeline 提前退出的实际统计与输出对象复用。
- 公共 ABI 测试保持 `VG_ERROR_UNSUPPORTED`、空 Submission、源/目标 bytes 不变的
  严格断言；没有新增 CMake lane，预期测试总数保持 Reference 38、Metal 72。

修复前已确认红灯：Reference 与非沙箱真实 Metal 的新 continuation 反例均在
`representation_epoch` 不变断言失败；Reference 公共 restricted mixed 拒绝失败；
前移 continuation 后、修正报告前，Reference 的零 transition 断言仍失败。
首次测试构造还暴露 multi-Node fixture 漏传 pending 输入；已先补齐，再获取上述
epoch 失败证据，避免把 fixture 缺陷误当作生产复现。

本节代码与测试由统筹代理实现，原设计审查代理进行有界只读复核；最终验证结果
另列于下，不覆盖前面历史证据。Linux SDK/真实 Vulkan 验证仍为独立 pending 门禁。

### 三项修复最终验收

- 独立复核：三项修复与新增测试均无阻塞项；只读审查，没有代替运行验证。
- `cmake --build --preset dev-reference -j4`、`cmake --build --preset dev-metal -j4`：通过。
- `ctest --preset dev-reference --output-on-failure`：**38/38**，17.83 秒。
- 非沙箱 `ctest --preset dev-metal --output-on-failure`：真实 Metal **72/72**，34.14 秒，无 skip。
- 非沙箱 `ctest --preset dev-metal -R '^(api.mixed-domain.(reference|metal)|conformance.reference-mixed-domain|vertical-slice.metal.mixed-domain)$' --repeat until-fail:20 --output-on-failure`：四项各 20 次，**80/80**，16.26 秒，无 skip。
- `python3 tests/vertical_slice/vulkan_capability_contract_test.py .`、Vulkan 测试源 C++20
  syntax-only、`git diff --check`：通过；三个 backend 的 legacy order/EffectGraph/
  first-Node 静态门禁零命中。没有运行 Linux SDK 或 Vulkan GPU。

调试中新增测试的两处构造问题已修正：host sentinel 改写需要 content-epoch 通知；
unsatisfied wait=1000 的计划使用合法 signal=1001，不能用 signal=2 在 assembler
阶段触发另一种拒绝。没有放宽 oracle、手工盖章 sealed plan 或降低平台门禁。

本次三项代码缺口已修复并有本地/真实 Metal 及独立审查证据；文档已同步。
**MD-5/6 整路线仍不能关闭：Linux SDK/真实 Vulkan 验证继续 pending。**
没有提交、推送或覆盖先前累积工作。
