# Phase E Gate Report（Research Alpha）

状态：`gate-recorded-per-adr-042`  
闭合性质：研究记录，不是产品关门  
盈亏曲线：`unmeasured`  
Vulkan：`compile-review-only`

本报告判断路线图 §6 对 Research Alpha 的退出材料。原文退出句不改写。
证据形状沿用 ADR-024/042：Metal+reference 真跑，Vulkan 永不计入 `passed`。
`HostAssisted` / `EmulatedDevicePass` / `Unsupported` / `Deferred` / `unmeasured`
是合法行。D 的研究记录（ADR-041）不是本阶段的自动入口。

## 产物对照（12 §6）

| 产物 | 本机状态 |
|---|---|
| 版本化公共 C ABI | v1.0 + `vgGetApi`；函数表仍是最小集（create/destroy/enumerate）。研究表面是 C++ DeviceHAL + `vg-exp`。 |
| compiler / tool binaries | 源码 CMake 目标存在；**无发行包装**（范围限制） |
| Metal / Vulkan adapters | Metal 真跑；Vulkan compile-review-only |
| conformance | 分散 ctest + `phase-a`–`phase-e` runner；无单独名为 `conformance` 的聚合 target |
| E001–E018 状态 | 本表 18 行 |
| sample workloads | 现有 fixture / vertical slice；无独立可分发 workload 包（范围限制） |
| 完整 run bundle | `vg-exp phase-e` / `benchmark` 写出 `artifacts/runs/` |
| architecture paper/report | [architecture-research-alpha.md](architecture-research-alpha.md) 骨架 |
| P0 风险 | [p0-risk-register.md](p0-risk-register.md)：关闭或有范围限制 |
| 文档与代码一致 | START / README 事实状态已更正；vg-project 原文不改写 |
| 干净 checkout 复现 | [external-repro-runbook.md](external-repro-runbook.md) |

## E001–E018

E004 的 B 时代行 `E004-access-certificate.json` 仍是历史结果（DiscoverThenLease
退化成 Universe）。`phase-e` 只加载 `E004-discovery-revisit.json`。

| ID | 分类 / 状态 | 来源 | ctest |
|---|---|---|---|
| E001 | reference-complete | A gate；ADR-003/007 | `model.phase-a` |
| E002 | CachedObject（Metal+ref） | B gate；ADR-028 | `vertical-slice.metal.pointer-graph`, `compiler.compute-package-golden` |
| E003 | reference-complete（host bounded model） | A gate；无 GPU litmus | `model.phase-a` |
| E004 | HostAssisted（发现集可小于 Universe） | D gate；ADR-036 | `core.discovery`, `vertical-slice.metal.discovery` |
| E005 | Metal DevicePass；峰值字节 unmeasured | C 实验已实现；C gate 仍 `not-closed`；ADR-032 | `vertical-slice.metal.consume-input` |
| E006 | reference-complete | A gate | `conformance.phase-a` |
| E007 | Metal Direct 表解引用 | B gate；ADR-029 | `vertical-slice.metal.indexed-binding` |
| E008 | Metal DevicePass | C 实验已实现；C gate 仍 `not-closed`；ADR-031 | `vertical-slice.metal.sample-facet` |
| E009 | Metal DevicePass（机制正确性；百万实例 unmeasured） | B gate；ADR-026 | `vertical-slice.metal.tier1-indirect`, `vertical-slice.metal.cull-compact` |
| E010 | Metal ICB DevicePass；回退 EmulatedDevicePass；ref Serialized | D gate；ADR-038 revisit | `unit.tier2-oracle`, `vertical-slice.metal.tier2-nodes` |
| E011 | proxy 字节 + 硬拒绝；sparse Unsupported | D gate；ADR-037 | `core.working-set`, `vertical-slice.metal.working-set` |
| E012 | 3/4 图形状 DevicePass；其余 Deferred/Unsupported | B gate；ADR-027 | `vertical-slice.metal.effect-dag` |
| E013 | Metal DevicePass；非 C 硬门槛 | ADR-030/034 | `vertical-slice.metal.pipeline-classification` |
| E014 | 同环境 reference replay；跨后端语义对照 | D gate；ADR-040 | `capture.view`, `capture.view.cli` |
| E015 | reference-complete | A gate | `conformance.phase-a` |
| E016 | Metal DevicePass + 背压；峰值字节 unmeasured | C 实验已实现；C gate 仍 `not-closed`；ADR-033 | `vertical-slice.metal.representation-churn` |
| E017 | HostAssisted（overflow + 下一提交） | D gate；ADR-039 | `core.envelope-continuation`, `vertical-slice.metal.envelope-continuation` |
| E018 | reference-complete | A gate | `core.unit` |

Vulkan 对全部 18 行都是 `compile-review-only`，由
`python3 tools/vg-exp/vg_exp.py phase-e --build-dir build/dev-metal` 固定写出。

## 明确未做 / 未测 / 范围限制

- 盈亏 / hitch / 百万实例 / 物理 RAM 压力：`unmeasured`
- Phase C 整体退出（含 basic raster oracle 作为 C 关门证据）：仍 `not-closed`
- GPU litmus（E003 P0）：未做，见 P0 登记
- DelegatedEnvelope、OS 驻留计数、Vulkan DGC 执行、发行包装：不在本机范围
- 公共 C ABI 未扩展到完整语义表面

P0 benchmark 入口：`python3 tools/vg-exp/vg_exp.py benchmark --build-dir build/dev-metal`
（E010 / `vertical-slice.metal.tier2-nodes`，证据等级 P0，仅 host wall-clock）。

官方 runner：`artifacts/runs/20260823T230413Z-PHASEE-5868a9795a30-f0105c3c7d770ce9-775e8790`
（25/25 执行行 passed；Vulkan 18 行 compile-review-only）。

机器可读状态见 [phase-e-gate.json](phase-e-gate.json)。
对外复现见 [external-repro-runbook.md](external-repro-runbook.md)。
