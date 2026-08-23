# HostAssisted 边界清单（Phase D / ADR-041）

这份清单只写 **VG 语义合同在现有 Metal / reference 适配器上的断点**。
它不是 Metal API 对照表，也不是把 `HostAssisted` 改写成 `Direct` 的关门辞。

Vulkan 五行都是 compile-review-only（ADR-024），不进入本清单的执行栏。

## 五问

| 问题 | 本机答案 | 分类 | 证据 |
|---|---|---|---|
| 发现是否必须回主机？ | 必须。`discover_reachable` 在主机读 12 字节 `PointerRef`，可达集回到主机后再提交。没有 GPU discovery kernel，也没有同提交 compact。 | `HostAssisted`，不是 `DevicePass` | E004 重访；ADR-036 |
| 多节点是否必须分桶？ | 必须用「bucket compute + 每类一条 indirect」，才能在现有 API 上选 ≥2 个预授权 Node。本机没有跑过 ICB 多管线 select。 | Metal：`EmulatedDevicePass`；reference：`Serialized` | E010；ADR-038 |
| 续跑是否必须第二次提交？ | 必须。portable 机制是 overflow buffer + 下一提交。没有 DelegatedEnvelope，也不许静默加大 `envelope_task_quota`。 | `HostAssisted` | E017；ADR-039 |
| 工作集是否只有 proxy？ | 是。`requested` / `committed` / `proxy` 都来自 `allocation->size`，原因写明 proxy。本机没有可当真理的 OS 驻留计数器。Metal sparse = `Unsupported`。 | 预算检查是硬拒绝；数字是 proxy | E011；ADR-037 |
| 抓包跨后端是否只是语义对照？ | 是。同环境回放只在 cpu-reference。Metal↔Vulkan 只对照稳定 ID / IR hash / epoch / fault taxonomy，不声称两边都执行过。 | 跨后端：语义对照，不是 dual execution | E014；ADR-040 |

## 不是本机断点的两件事

- **B 时代 E004**：`DiscoverThenLease` 退化成 Universe 全扫，仍是历史结果（`E004-access-certificate.json`）。D 的重访没有改写那一行。
- **地址图 ≠ 本提交驻留**：E011 证明手选租约或发现租约可以小于同一 Arena 的 Universe。这是语义集合，不是 OS 页迁移。

## 未决问题对照（12 §8，原文不改）

| §8 | 本清单结论 |
|---|---|
| 3 Certificate composition | 种子拓扑证书能盖住本次走图见证。跨间接调用 / Task child 的合成仍未做。 |
| 8 Discovery publication | GPU-side set 不经 host 直接供 adapter residency：**现有 API 上没做到**。研究结论就是必须回主机。 |
| 10 Envelope continuation | 选出 portable overflow queue + 下一提交。预授权 DelegatedEnvelope：**本机没有**。 |

## 曲线

盈亏 / 成本曲线 **未测**。已有的是小夹具计数，不够画 break-even：

- E004：同一 Arena 上 2/4 与 1/4 两档可达，没有 1%–100% 扫描。
- E010：8-task 均匀 4+4、偏斜 7+1，没有 hitch 曲线。
- E011：16/32/64 字节夹具，没接近物理 RAM。
- E014：正确性 / 哈希，无性能点。
- E017：3-task 链、quota 1 与 8，不是大规模展开。

`discovery_host_ns` 随跑次变化，不当作稳定成本点。
