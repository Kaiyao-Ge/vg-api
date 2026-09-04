# MD-4 Metal mixed-domain 交付记录

日期：2026-09-03。范围：整改报告 §9.2.6、9、11、13，合同依据为
[ADR-054](../decisions/ADR-054-mixed-domain-execution-schedule.md)。

## 实现与边界

MD-4 在 MD-1 的密封 ExecutionSchedule、MD-2 的公共 Stage 6/7 transition
合同、MD-3 的 Reference 语义基线之上，开放 Metal canonical Compute 与 built-in
Raster 的同图执行。它不是整个 mixed-domain 路线或所有后端的完成声明。

- 所有 Metal plan-driven Task（native、HostAssisted、Raster）走同一个
  component/wave scheduler，按完整 NodeRef 选择 package；不重新读取 EffectGraph
  推导调度，也不投影到第一个 Node。
- 当前每个 native Task 使用独立 command buffer 并等待完成。该实现保留 Core
  的并行资格，但实际执行是明确报告的保守串行 fallback，不宣称 GPU wave 并行。
- Stage 6 transition 成本按对应 producer wave 的实际 package 类型计算。
  Submission 计数从零按实际操作累计，区别于 CompiledPlan 的计划成本；取消、
  Timeline 提前退出、提交前失败不虚报等待，已经提交后失败的命令保留真实成本。
- Compute→Raster 由真实 compute/render 命令验证。当前测试设备的 64-bit atomic
  无法原生编译，因此 Raster→Compute atomic 用例明确断言 HostAssisted，验证结果
  与顺序，但不作为原生 render→compute fence 的证据。
- `published_tasks` 是完整 Envelope-filtered canonical 序列。mixed/raster
  publication 走 host；compute-only 仍使用原有 GPU ring。发布失败阻止 Task 执行。
- 失败取消未启动的结构后继，独立分支继续；主 FaultRecord 按 canonical rank
  选择，保留 host fault/trace/witness，poison 不把 attempted trace 当作已写输出。
- Timeline 为 submission-wide，所有允许执行的 Task 成功后才 signal。
  representation operation 每次 submission 执行一次，Allocation/Facet hold
  保持 SR-5 顺序并在同步完成后释放。
- restricted user-raster mixed、native 与 HostAssisted Compute package 混用仍在
  Stage 6 精确整计划 `Unsupported`；SceneRoot narrowing、同 NodeRef 跨域拒绝保留。
  Vulkan、公共 C ABI、`VgTaskRecordV2`、ring schema 与资源生命周期没有扩张。

## 文件与测试归属

| 文件 | MD-4 内容/归属 |
|---|---|
| `src/backends/metal/metal_device_hal.mm` | 生产 compile、单一 scheduler、publication、transition 和物理统计 |
| `tests/vertical_slice/metal_mixed_domain_conformance.cpp` | 真实 assembler + Metal/Reference oracle；替代 MD-3 临时 mixed guard target |
| `tests/vertical_slice/metal_task_timeline_test.cpp` | 既有 plan-driven compute/raster、调度计数与边界回归 |
| `tests/support/assembled_plan_fixture.h` | 真实 assembler fixture 传递 Timeline/Envelope 等测试输入 |
| `CMakeLists.txt` | mixed-domain conformance 注册 |

测试覆盖 C→R、R→C HostAssisted、独立 mixed components、全 HostAssisted
component/wave 实际执行顺序、完整 publication、repeat submit/hold 释放、
representation 一次执行、transition 篡改的 Stage 7 副作用前拒绝、合法不同
NodeRef 的 restricted shader 整图拒绝、失败后继取消/主 fault/两种 poison、
Timeline 成功与失败、producer wave 和实际成本反例。

没有通过手工 stamp plan/package、篡改 Arena 或新增生产故障 hook 制造通过证据。
全 HostAssisted atomic 没有现成可控 logical-fault 注入点，采用其实际 trace 证明
经过唯一 scheduler，加同一 scheduler 的 mixed failure 矩阵组合覆盖。
硬件 command-buffer/device 故障分支经过代码审阅，未做硬件故障注入。

## 审查与验收

实施、独立只读审查及父代理复核分别进行。审查发现并关闭了全计划统计污染
单 wave、HostAssisted 快路径绕过失败调度、publication 失败被覆盖、representation
与失败命令统计遗漏、提前返回继承计划计数等问题。最终独立审查无剩余 MD-4 blocker。

验证命令：

```sh
cmake --build --preset dev-reference -j4
ctest --preset dev-reference --output-on-failure
cmake --build --preset dev-metal -j4
ctest --preset dev-metal --output-on-failure
ctest --preset dev-metal -R '^vertical-slice\.metal\.(mixed-domain|effect-dag|task-graph-raster|envelope-continuation)$' --repeat until-fail:20 --output-on-failure
git diff --check
rg -n 'plan\.task_order|validated_effect_graph|effect_graph_deterministic_order|classify_effect_graph_shape|resolved_nodes\s*\[\s*0\s*\]|per_node_packages\s*\[\s*0\s*\]|resolved_nodes\.front\(|per_node_packages\.front\(' src/backends/metal/metal_device_hal.mm
```

父代理独立复跑 Reference 37/37、非沙箱真实 Metal 70/70（32.02 秒）；
四个真机定向用例各重复 20 次、共 80 次执行全部通过（14.80 秒）。
两套全量均包含 `docs.check`，交付文档写入后另行复验通过。
静态搜索零命中，`git diff --check` 通过。沙箱内找不到 MTLDevice 的 skip
不计入真机验收。

## 整改台账与后续依赖

| 项目 | 负责人 | 状态与证据 | 剩余边界 |
|---|---|---|---|
| MD-1/2/3 前置 | 前序工作包 | 当前工作树保留；本轮全量回归通过 | 本记录不重新声明其独立交付范围 |
| MD-4 实现 | MD-4 子代理 | 单一 schedule、per-Node、真实统计及相应反例 | 保守串行与上述 Unsupported narrowing |
| MD-4 审阅 | 独立审查代理 + 父代理 | 当前包 blocker 已关闭 | 不代替硬件故障注入 |
| 后续 Vulkan / 跨后端收口 | 待单独授权 | 本轮未实施 | 继续保持 concrete Raster whole-plan Unsupported；不得将本轮 Metal 绿灯推广为 Vulkan 完成 |
| §9.3 文件治理/并行优化 | 后续独立工作包 | 本轮不实施 | 不以收尾名义扩张 MD-4 |

本轮不创建提交、不推送；验收针对共享 `main` 工作树中的累积改动。
