# Phase D Gate Report

状态：`gate-recorded-per-adr-041`  
盈亏曲线：`unmeasured`（样本不足，不编点）

本报告判断路线图 §5 对 Dynamic Graph / Residency Research 的退出材料。
原文退出句不改写。证据形状沿用 ADR-024/035：Metal+reference 真跑，
Vulkan compile-review-only。`HostAssisted` / `Unsupported` 是合法结论。

| 产物 | 状态 |
|---|---|
| E004 重访（发现集 < Universe） | recorded，`HostAssisted` |
| E010 多 Node lowering | recorded，Metal ICB `DevicePass`（失败回退分桶 `EmulatedDevicePass`） / reference `Serialized` |
| E011 本提交工作集 | recorded，proxy 字节 + sparse `Unsupported` |
| E014 抓包可视 / 同环境回放 | recorded，跨后端仅语义对照 |
| E017 信封续跑 | recorded，`HostAssisted` overflow + 下一提交 |
| break-even curves | **未测** |
| HostAssisted 边界清单 | [host-assisted-boundary.md](host-assisted-boundary.md) |
| NativeContractResearch v1 | [native-contract-research-v1.md](native-contract-research-v1.md) |

这是研究关门，不是产品关门，也不是 Phase E 的入口。
Phase E 由 [ADR-042](../decisions/ADR-042-phase-e-evidence-policy-and-external-reproducibility.md)
单独记录，见 [phase-e-gate.md](phase-e-gate.md)。

## 五门实验

B 时代 `E004-access-certificate.json` 仍是历史行（DiscoverThenLease = Universe
全扫）。D 用 `E004-discovery-revisit.json`。

| Gate | 分类 | ADR | Task | ctest |
|---|---|---|---|---|
| E004 discovery revisit | HostAssisted；discovered 2&lt;4 与 1&lt;4 | ADR-036 | TASK-D2 | `core.discovery`, `vertical-slice.metal.discovery` |
| E010 heterogeneous nodes | ICB DevicePass（4+4 / 7+1 多重集）；分桶为回退 | ADR-038 | TASK-D4 | `unit.tier2-oracle`, `vertical-slice.metal.tier2-nodes` |
| E011 working set | 手选 16B/预算 64 过；Universe 32B/预算 16 拒；sparse Unsupported | ADR-037 | TASK-D3 | `core.working-set`, `vertical-slice.metal.working-set` |
| E014 capture view | reference 哈希稳定；消费后精确拒绝；Metal↔Vulkan 语义对照 | ADR-040 | TASK-D6 | `capture.view`, `capture.view.cli` |
| E017 envelope continuation | quota 1 → 发布 1 + leftover 2；无令牌不偷；大 quota 一次过 | ADR-039 | TASK-D5 | `core.envelope-continuation`, `vertical-slice.metal.envelope-continuation` |

Vulkan 对全部五行都是 `compile-review-only`，由
`python3 tools/vg-exp/vg_exp.py phase-d --build-dir build/dev-metal` 固定写出，
从不与 `passed` 混在一起。

## 明确未做 / 未测

- 盈亏曲线：没有 1%–100% 发现扫描、没有 hitch 曲线、没有物理 RAM 压力。
- E014 `dynamic-graph`：仍 blocked（发现 API 有了，但没有发现子集的抓包）。
- E014 `driver-update`：skipped（没有第二套驱动实验室）。
- GPU-side discovery 不经 host、ICB/DGC 原生 select、DelegatedEnvelope、
  OS 驻留计数器：见 NativeContractResearch v1，本机不实现。

官方 runner：`artifacts/runs/20260823T194226Z-PHASED-8df2d090a607-f0105c3c7d770ce9-51d45f62`
（10/10 执行行 passed；Vulkan 五行 compile-review-only）。

机器可读状态见 [phase-d-gate.json](phase-d-gate.json)。
