# TASK-B0: DeviceHAL, Capability Snapshot, and ExecutionPlan

状态：已完成（Phase B foundation）

## 交付内容

- 新增版本化内部 `DeviceHal` interface。
- 新增 immutable `CapabilitySnapshot`、`ExecutionPlan`、`CompiledPlan`、`Submission`。
- 新增 `LoweringReport`/event 分类与 canonical JSON serialization。
- 计划验证 generation-independent canonical IR、capability、timeline 顺序和
  publication state；不携带 backend handles 或未验证用户指针。
- 新增 reference DeviceHAL 作为确定性 baseline。

## 边界

本任务没有修改 public C ABI，也没有实现 Metal/Vulkan allocation、shader
codegen 或 command submission。

