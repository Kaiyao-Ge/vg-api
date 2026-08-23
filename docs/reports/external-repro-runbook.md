# External reproduction runbook（Research Alpha / ADR-042）

另一名研究者或 Agent 从干净 checkout 应能完成：构建 → 一致性测试 →
一个 P0 benchmark。本页只写本仓库里真实存在的命令。Vulkan 真机不是门槛。

## 环境

- CMake ≥ 3.25，Ninja，Python ≥ 3.10，C11 / C++20
- macOS + Xcode command-line tools：Metal adapter（`dev-metal`）
- 任意主机：CPU reference（`dev-reference`）
- Linux + Vulkan headers/loader：可选 `dev-vulkan`（本开发机不执行）

## 1. 构建 reference 并跑 portable conformance

```bash
cmake --preset dev-reference
cmake --build --preset dev-reference
ctest --preset dev-reference --output-on-failure
python3 tools/vg-exp/vg_exp.py phase-a --build-dir build/dev-reference
```

期望：`phase-a` 五门 reference 行 passed。这覆盖 E001/E003/E006/E015/E018。

## 2. 构建 Metal 并跑 adapter smoke

```bash
cmake --preset dev-metal
cmake --build --preset dev-metal
ctest --preset dev-metal --output-on-failure
```

期望：已注册的 Metal vertical slice 与 `tooling.phase-b-runner`、
`tooling.phase-d-runner`、`tooling.phase-c-runner`、`tooling.phase-e-runner`、
`tooling.phase-e-benchmark` 在本机通过。首次配置后必须 reconfigure，
以便 `cmake/phase-e-e*.cmake` 进入构建。

## 3. 聚合 18 项实验状态

```bash
python3 tools/vg-exp/vg_exp.py phase-e --build-dir build/dev-metal
```

期望：写出 `artifacts/runs/*-PHASEE-*`；`summary.json` 中
`experiment_count == 18`，执行行 `failed == 0`，
`vulkan_status == compile-review-only`（18 条 Vulkan 样本，不与 passed 混计）。
E004 解析为 `E004-discovery-revisit.json`。

人类可读对照：[phase-e-gate.md](phase-e-gate.md)。

## 4. 一个 P0 benchmark

```bash
python3 tools/vg-exp/vg_exp.py benchmark --build-dir build/dev-metal
```

包装 `vertical-slice.metal.tier2-nodes`（E010）。证据等级 **P0**：
只说明命令可运行，记录 host wall-clock，不声称 `gpu_ns`、hitch 或盈亏曲线。

## 5. 文档检查

```bash
cmake --preset docs
cmake --build --preset docs
ctest --preset docs --output-on-failure
```

## 不应期望的事

- 不得把 Vulkan compile-review-only 当成执行通过
- 不得要求发行包装或独立 workload 包
- 不得把 Phase C `not-closed` 当成已关门
- `artifacts/runs/` 不入库；每次干净 checkout 需本地重跑
