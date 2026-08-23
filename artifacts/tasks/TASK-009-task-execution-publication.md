# TASK-009: TaskGraph, PublicationRing, Execution, and Quota

状态：已完成（Phase A / PortableCore）

## 交付内容

- PublicationRing 增加 checked `publish_task`、abort recovery 和 overflow 诊断。
- TaskGraphBuilder 支持 publication slot 接入、task/payload quota 和 quota 负向验证。
- sealed graph 增加 publication-before-execution 检查。
- 增加 generated `TaskRoot` schema 到 checked `TaskRecord` 的绑定转换。
- E003 风格测试覆盖完整发布、ring wrap/overflow、builder 接入和 schema mapping。

## 边界

本任务不实现 GPU-generated continuation、delegated envelope 或公共 C ABI 扩展；
这些能力需要后续阶段的 authority/quota 合同。

