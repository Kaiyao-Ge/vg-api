# TASK-011: Phase A Experiment Definitions and Evidence Runner

状态：已完成（Phase A / PortableCore）

## 交付内容

- 新增 E001、E003、E006、E015、E018 machine-readable definitions。
- 每项实验增加 hypothesis/workload、variants、metrics、protocol、correctness、
  requirements 和 judgement，并配套实验 README。
- `tools/vg-exp/vg_exp.py phase-a` 逐项运行 reference CTest，生成标准 run bundle、
  samples、summary、CSV、report 和 manifest hashes。
- runner 明确将 NativeAdapter 结果标记为 `deferred`，不伪装成零成本或通过。
- schema/tool tests 验证五项定义完整且 runner 五项全部通过。

## 边界

本任务不声称完成 Metal/Vulkan 实验；其结果属于 PortableCore reference evidence。

