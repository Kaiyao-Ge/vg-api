# TASK-008: Certificate, Witness, Fault, and Poison Integration

状态：已完成（Phase A / PortableCore）

## 交付内容

- reference executor 执行前验证 inferred effects 是否被 sound certificate 覆盖。
- ExecutionResult 返回实际 `AccessWitness`、missing effects、structured fault、
  poison state 和 `outputs_valid`。
- allocation lookup 同时验证 generation、representation epoch 和 bounds。
- 区分 `Poisoned` 与 `PartiallyProduced`，明确 fault 不回滚既有写入。

## 验证

现有 conformance 覆盖成功 witness、certificate omission、stale allocation；
model witness 覆盖 missing/unused ranges。E006/E015 的完整 fixture runner 留给
TASK-011，但其 reference 语义已具备。

