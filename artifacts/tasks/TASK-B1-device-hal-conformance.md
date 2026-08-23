# TASK-B1: DeviceHAL Capability and Conformance Harness

状态：已完成（Phase B foundation）

## 交付内容

- 新增 `conformance.device-hal` 数据驱动式最小 harness。
- 覆盖 capability/version validation、reference compile/submit、输出正确性、
  LoweringReport、hidden HostAssisted 识别和 invalid timeline rejection。
- 复用 canonical IR 与 reference execution oracle；为后续 Metal/Vulkan adapter
  保留相同的 plan/report/conformance 边界。

## 验证

普通 CTest 当前为 `14/14 passed`。Metal/Vulkan adapter conformance 尚未声称
完成，属于后续 B2/B3/B5/B6 工作。

