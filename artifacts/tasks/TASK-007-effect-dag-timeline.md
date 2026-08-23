# TASK-007: Effect DAG, Happens-Before, and Timeline

状态：已完成（Phase A / PortableCore）

## 交付内容

- 为 `EffectGraph` 增加 `Explicit`、`InferredConflict`、`Timeline`、`Publication`
  edge reason，并保留 cycle detection。
- 实现同 allocation、同 representation epoch、重叠 byte range 的冲突分类；
  read/read 不冲突，其余写入、atomic、publish 组合建立 hazard。
- `TaskGraphBuilder::seal` 根据 task effects 推导顺序边，并将带原因的 DAG
  写入 sealed `TaskGraph`。
- 增加 happens-before 可达性验证，缺少冲突顺序时返回明确诊断。
- 增加 Timeline wait 校验和带 required value 的 Timeline edge；保持严格单调
  signal，拒绝 rollback 与未满足 wait。
- 增加重叠 write/read、read/read 并行、缺失 happens-before、cycle、Timeline
  rollback/unsatisfied wait 与 edge reason 测试。

## 边界

本任务不实现 Metal/Vulkan barrier lowering、跨进程 OS semaphore、动态 effect
discovery 或公共 C ABI 扩展；这些属于后续 Phase/任务。

## 验证

普通构建和 CTest：12/12 通过。Sanitizer 构建也应执行同一测试集作为门禁。

