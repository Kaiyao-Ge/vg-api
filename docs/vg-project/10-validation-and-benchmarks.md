# 10 Validation and Benchmark Policy

本文件规定如何判断实现正确、比较公平，以及什么证据足以支撑架构结论。

## 1. 验证金字塔

```text
                 End-to-end workloads
              Cross-backend differential
           DeviceHAL conformance + fault
        IR/effect/model/property-based tests
     ABI/layout/unit/static analysis/sanitizers
```

下层失败时不得用上层 demo 成功覆盖。三角形只能证明一条窄路径能工作。

## 2. 测试类别

| 类别 | 运行频率 | 目的 |
|---|---|---|
| unit/schema/ABI | 每次改动 | 快速定位布局与逻辑 |
| model/property | 每次改动/夜间加量 | 生命周期、epoch、effect 状态空间 |
| CPU conformance | 每次改动 | portable semantics |
| Metal smoke/conformance | M1 可用时 | adapter correctness |
| Vulkan smoke/conformance | server CI/手动 | adapter correctness |
| performance smoke | 每日/里程碑 | 捕捉大回归，不做论文结论 |
| formal benchmark | 预注册、受控环境 | 架构结论 |

## 3. Core conformance suite

同一测试向每个 DeviceHAL 提交 canonical plan，覆盖：

- object create/destroy/generation；
- allocation/map/flush/address/relative ref；
- schema/root/pointer graph；
- Region linear/sample/storage/attachment；
- Task build/seal/publish/quota；
- effect read/write/atomic/happens-before；
- Timeline monotonic/wait/timeout；
- certificate modes；
- Graph/RepresentationEpoch；
- ConsumeInput proof；
- poison/device lost；
- lowering classification；
- capture/replay。

Backend 不支持的 optional test 必须返回精确 capability reason。必需能力 unsupported 则 adapter 不达到该 profile。

## 4. Negative tests

至少包括：integer-forged pointer/capability、out-of-bounds、overflow、misalignment、stale generation、use-after-retire、mutate-after-seal、partial Task、undeclared write、certificate omission、epoch mismatch、illegal consume、timeline rollback、Node not in envelope、quota overflow、unsupported facet、external ownership misuse。

每个负例声明预期错误 phase/code。仅“没有 crash”不算通过。

## 5. Model checking 与 property tests

用小状态模型枚举 allocation/task/epoch/timeline 操作，验证：

- retired generation 永不重新可见；
- in-flight reference 对应 backing/facet 不被回收；
- happens-before 无边则并发写被拒绝/报告；
- certificate 是实际 trace 的超集；
- ConsumeInput 后旧 generation 永久无效；
- fault 后 poisoned region 不被当作 valid output。

随机测试必须保存 seed 和最小化 counterexample。

## 6. Backend differential

同一 canonical input 在 reference、Metal、Vulkan 运行。比较输出、effect trace 摘要、Task count、epoch publication 与 fault taxonomy。backend 可有不同 lowering，但语义事件必须可映射。浮点/采样差异按预注册 tolerance。

## 7. 公平 baseline 原则

VG 不是通过比较糟糕的 Vulkan/Metal 写法获胜。每个性能实验至少包含：

- idiomatic backend baseline；
- 与 VG 最接近的现代能力 baseline（如 BDA/bindless/ICB/indirect）；
- 必要时传统 API-style baseline，用于解释历史开销而非唯一对手；
- CPU reference 只作 correctness，不作 GPU 性能对手。

保持一致：算法、可见工作、输入、输出精度、shader optimization、batch size、frames in flight、同步正确性、allocation lifetime、pipeline warm state。

## 8. 成本归因

必须分开：

- application data preparation；
- VG validation/graph compile；
- adapter lowering；
- backend object create/update；
- command encode/submit；
- GPU useful work；
- GPU translation/discovery/transform work；
- host wait/readback；
- pipeline/shader compile；
- allocation/residency；
- capture/instrumentation。

若某阶段被缓存，cold 与 warm 都报告。异步工作最终归入触发它的 submission 或 background category。

## 9. 指标

核心指标：median、p5/p95、MAD、bootstrap 95% CI、sample count。吞吐同时报告 latency。内存报告 peak/steady/temporary。CPU 报 wall 与可得的 CPU time。GPU 报 timestamp 和执行 pass。

不要默认只用平均值；长尾是 pipeline compile、residency 和动态 Task 的核心现象。

## 10. 稳定性与异常值

预热后检查 rolling median、clock/thermal、background load。异常值规则在实验前固定：例如系统调度中断可标记，但保留原始值。汇总同时给 all-samples 与 policy-filtered。设备错误、OOM、timeout 是结果，不能删除后只统计成功样本。

## 11. 回归阈值

CI threshold 应考虑噪声：correctness/ABI 零容忍；CPU deterministic microbenchmark 可用 5-10%；GPU 小于噪声的变化标记 inconclusive。正式阈值由基线 run 的方差生成并版本化，不把任意单次 3% 当回归。

## 12. “无隐藏成本”检查

每次 submit 对账：

```text
VG plan operations
  + backend commands/passes
  + descriptors/facets/pipelines
  + barriers/timeline waits
  + copies/transforms
  + host round-trips
  + allocation/residency actions
```

GPU capture/validation 抽样检查 report 是否遗漏。若无法观测驱动内部成本，报告写 `unknown_vendor_internal`，不能写零。

## 13. 性能结论等级

| 等级 | 可说什么 |
|---|---|
| P0 | demo/单样本，只说明可运行 |
| P1 | 同机重复 microbenchmark，说明局部趋势 |
| P2 | 公平 baseline + CI + lowering，说明该机器/driver 结论 |
| P3 | Metal/Vulkan 多平台一致或解释差异，支持 portable architecture 主张 |
| P4 | 独立复现/原生 DeviceHAL，支持更广硬件主张 |

文档措辞不得超过证据等级。

## 14. Sanitizer 与工具

Host：ASan/UBSan/TSan（适合目标分别运行）、static analyzer、fuzz ABI/IR parser。GPU：reference bounds/race、Metal validation、Vulkan validation、shader instrumentation、backend capture。工具禁用时记录原因。

## 15. 发布门槛

一个里程碑 release 必须：

- unit/core conformance 全绿；
- 两个 adapter 的 supported profile 全绿；
- 无未分类 validation error；
- ABI/layout compatibility 报告；
- benchmark 原始 run bundle；
- 所有性能主张带等级和限制；
- 已知 Unsupported/HostAssisted 列表；
- 文档链接与规范版本校验通过。

## 16. 拒绝发布条件

- adapter 静默全局同步或 host readback；
- stale/epoch/certificate 错误可能读写错误对象；
- Task 发布协议没有 litmus；
- baseline 算法/精度不同；
- 只保留有利样本；
- 无法从结果定位代码、配置、driver；
- 将 Metal/Vulkan adapter 结果声称为原生 KMD/硬件能力。

