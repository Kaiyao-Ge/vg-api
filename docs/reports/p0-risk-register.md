# P0 风险登记（Phase E / ADR-042）

路线图 `12-roadmap-and-risks.md` §7 的 P0 是否定核心语义或造成静默错误
的风险。Research Alpha 的退出要求每条关闭，或写下明确范围限制。
本表不发明尚未存在的测试。

| 风险 | Owner | 已有缓解 / 测试 | 状态 |
|---|---|---|---|
| AccessCertificate 不 sound | Core/Compiler | E004 B+D（`vertical-slice.metal.access-certificate`, `core.discovery`, `vertical-slice.metal.discovery`）；E006 witness（`conformance.phase-a`）；伪造租约拒绝 | **缓解已记录**。发现路径仍是 HostAssisted；sound 上界在 reference/Metal 夹具上可拒绝漏项。无跨间接调用合成（12 §8 未决 3），标范围限制。 |
| Epoch 回收竞态 | Core | `model.phase-a`；FacetPool generation（`vertical-slice.metal.checked-facet-generation`）；`core.unit` | **缓解已记录**。模型序列是 1000 次，不是 catalog 的 100k。范围限制：无独立回收竞态压力 harness。 |
| Task publication 弱内存错误 | Core/Backend | `model.phase-a` / TASK-003 host bounded model（release/acquire、overflow、quota） | **范围限制，未关闭**。没有 GPU litmus，没有 Metal/Vulkan 弱内存原语实验。不得把 host 模型写成设备内存模型证明。 |
| Facet 在途偷换 | Core/Backend | RepresentationEpoch + slot generation；`vertical-slice.metal.representation-layer`, `checked-facet-generation`, `representation-churn` | **缓解已记录**。峰值字节 unmeasured；Phase C 未整体关门。 |
| Adapter 弱化同步 | Backend | E012 `vertical-slice.metal.effect-dag`（3/4 形状 DevicePass） | **缓解已记录，范围限制**。cross-queue / representation-transition / external-present 为 Deferred。无跨后端 differential gate。 |
| ABI/schema 分裂 | Compiler/API | 单一 schema generator；`schema.generate`；`abi.c` / `abi.cpp` / `abi.facet-token`；`VG_API_VERSION_1_0` | **缓解已记录，范围限制**。无独立 ABI compatibility 报告文件；public C 表面仍是最小函数表。 |
| Fault 被当成功 | Core/Backend | E015 `conformance.phase-a`（partial-output、stale epoch、certificate fault、capture replay） | **缓解已记录，范围限制**。adapter 级 fault 与 reference 无完整 parity gate；无设备 page fault 传播。 |

## 判定

七条均有 owner 与测试指针。一条（Task publication 弱内存）明确未关闭，
其余六条有缓解并带范围限制。这满足 ADR-042「关闭或有范围限制」，
不满足「全部 P0 已在硬件上退休」。

机器可读：[p0-risk-register.json](p0-risk-register.json)。
