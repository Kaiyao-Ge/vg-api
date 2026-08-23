# Architecture report skeleton（Research Alpha / ADR-042）

本文是可引用骨架，不是新性能论文。主张不得超过已有 gate 的证据等级
（`10-validation-and-benchmarks.md` §13）。不写 `gpu_ns`、hitch 或盈亏点。

## 1. 项目主张（charter G1–G6）

VG 研究一套以类型化 GPU 地址图、Region、Effect、不可变 Task、
AccessCertificate、RepresentationEpoch 和 ExecutionEnvelope 为中心的
公共语义，并把它降低到 CPU reference、Metal 与 Vulkan adapter。
当前产品是语义、适配代价、以及「只有原生驱动合同才可能去掉的协助」
三类证据，而不是替代 Metal/Vulkan 的更底层 API。

## 2. 三轨道

| 轨道 | 本机结论 |
|---|---|
| PortableCore | Phase A `reference-complete`：生命周期、发布、witness、fault、GraphEpoch |
| NativeAdapter | Metal 真跑；Vulkan compile-review-only（ADR-024） |
| NativeContractResearch | v1 已写出；本机不实现 UMD/KMD/firmware |

## 3. 已记录的适配边界

来源：[host-assisted-boundary.md](host-assisted-boundary.md)、
[native-contract-research-v1.md](native-contract-research-v1.md)、
A/B/C/D/E gate。

- 发现：主机遍历 pointer 图，可达集回到主机后再提交（`HostAssisted`）。
- 多 Node 选择：Metal 优先 GPU 编码 ICB（`DevicePass`）；失败则分桶 +
  每类一条间接 dispatch（`EmulatedDevicePass`）。不是原生驱动。
- 信封续跑：overflow buffer + 下一次 submit（`HostAssisted`）。无 DelegatedEnvelope。
- 工作集：字节来自 `allocation->size` 的 proxy；sparse `Unsupported`。
- 跨后端抓包：同环境 reference 回放；Metal↔Vulkan 仅语义对照。
- 线性指针图：CachedObject / 表解引用，不是「硬件里已经没有 binding」。
- ConsumeInput / SampleFacet / RepresentationEpoch：Metal 夹具可跑；
  Phase C 整体仍 `not-closed`；峰值字节 unmeasured。

## 4. 公共接口

版本化 C ABI 是 `VG_API_VERSION_1_0` / `vgGetApi`。函数表仍是运行时
创建、销毁与适配器枚举。研究与实验走 C++ DeviceHAL、IR 包和 `vg-exp`。
这是范围限制，不是完整应用 ABI。

## 5. 一致性与复现

- Portable：`ctest --preset dev-reference` 与 `vg-exp phase-a`
- Adapter：`ctest --preset dev-metal` 与 `vg-exp phase-b|c|d|e`
- 18 项状态：[phase-e-gate.md](phase-e-gate.md)
- P0 风险：[p0-risk-register.md](p0-risk-register.md)
- 命令：[external-repro-runbook.md](external-repro-runbook.md)

## 6. 明确不是本报告的结论

- VG 已经是比 Metal/Vulkan 更底层的 API
- 统一内存等于无限驻留
- ICB `DevicePass` 等于原生 DeviceHAL
- 未执行的 Vulkan 行等于跨平台性能可移植
- catalog 级工作负载已经测过

复访条件见 ADR-042。
