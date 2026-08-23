# TASK-010: Capture v1 and Reference Replay

状态：已完成（Phase A / PortableCore）

## 交付内容

- 扩展 capture 状态：allocation bytes/generation/representation/state、GraphEpoch
  引用、Timeline point、certificate、witness、fault/poison、source/compiler/schema
  hashes。
- 增加 capture content hash、schema version checks、IR hash checks 和 unknown
  required-field rejection。
- 增加 stable allocation-ID relocation map；禁止依赖 backend runtime address。
- `vg-replay` 现在反序列化 capture 后重新建立 Arena 并调用 reference executor，
  不再只是打印 IR hash。
- 增加 normal replay、fault/partial-output replay 和 witness round-trip conformance。
- 保留旧 `serialize(module, arena)` 与 `deserialize(..., Module*)` 兼容接口。

## 验证

普通构建/CTest 与 ASan/UBSan CTest 均应保持全绿；conformance 覆盖正常输出、
certificate replay、atomic fault/poison replay、epoch mismatch 和 stable relocation。

## 边界

本任务不实现跨设备 backend resource import、Metal/Vulkan capture 或外部 ownership
迁移；这些属于后续 NativeAdapter/Phase B 工作。

