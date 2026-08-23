# NativeContractResearch v1

轨道：`NativeContractResearch`（`docs/START.md` §3）。  
性质：研究笔记。**不是 KMD / firmware 开工单，本机也不会实现这些合同。**

读者对象：已经读过 [host-assisted-boundary.md](host-assisted-boundary.md)
和五门 D 实验定义的人。问题是：哪些能力 **只有** UMD / KMD / firmware
合同才可能去掉清单上的协助。

## 1. 本机已经停住的地方

现有 Metal / reference 适配器能诚实做到的上限：

- 发现是主机走图 + 回主机（`HostAssisted`）。
- 多 Node 选择：Metal 优先 GPU 编码 ICB（`DevicePass`）；ICB 失败才回退 bucket + per-Node indirect（`EmulatedDevicePass`）。Vulkan DGC 仍未执行。
- 信封溢出靠第二次 submit，令牌记在设备对象上的主机表。
- 工作集数字是 allocation 大小的 proxy；sparse 为 `Unsupported`。
- 跨后端抓包是语义对照。

这些不是实现偷懒，是现有用户态 API 的可见合同到此为止（01 §5.4：负面结果也算成功）。

## 2. 若要去掉协助，合同该从哪问

下面每一行都是 **研究问题**，不是「下一步写驱动」的任务。

### 2.1 发现集留在设备上（对应 12 §8 未决 8）

现有断点：可达集必须回到主机，才能盖证书、填租约、给下一次 submit。

要问的合同：GPU 能否在 **不经 host 读回** 的前提下，产出一份 adapter residency
能直接消费的可达集合（granule / 页 / allocation id）？谁冻结 `GraphEpoch`
与影响地址选择的输入？发现 Node 如何保证无业务副作用？

没有这份合同，把主机走图标成 `DevicePass` 是假的。

### 2.2 原生多管线 select（对应 E010）

现有断点（Metal 适配器侧）：GPU 编码 ICB 已是优先路径——同一 command buffer
里 reset → GPU encode → optimize → `executeCommandsInBuffer`，按 `node_index`
选预授权 PSO，host 不读回计数。未授权 Node 由 encode kernel 置位后拒绝。
Vulkan DGC 仍是 compile-review-only。Tier3（GPU 发明 Node、涨信封）仍拒绝。

还要问的合同：DGC 能否在可执行 Vulkan 上做同一件事；ICB 命令类型是否覆盖
后续需要的 draw / blit；失败回退不得被标成 `DevicePass`。

读回计数再 host 重编码，只能标 `Serialized` / `HostAssisted`。

### 2.3 本提交驻留计数器（对应 E011 / 06 §10）

现有断点：`requested` / `committed` / `proxy` 都是 `allocation->size`。
统一内存不是无限内存，但本机没有可当真理的 OS 驻留计数。

要问的合同：谁暴露「这一提交实际占住的字节」，并且能和语义租约对账？
discrete GPU 上的可驱逐集合如何与 `WorkingSetLease` 对齐？
Vulkan sparse 若要做，必须是 **显式 map/unmap**，不能写成自动 page fault。

### 2.4 预授权信封续跑（对应 E017 / 12 §8 未决 10）

现有断点：overflow buffer + 下一提交；令牌在 `DeviceHal` 主机表。

要问的合同：`DelegatedEnvelope` 或等价预授权——谁在第一次 commit 时
批准「可以再发布 N 个 Task」，GPU 涨信封时如何保持可审核、不变成隐式全局队列？
与 publication ring 满（单环写不下）必须仍是两种错误。

固件微内核扩信封若只存在于模拟，分类不得写成 `DevicePass`。

### 2.5 可执行的跨后端抓包（对应 E014）

现有断点：同环境回放在 reference；Metal↔Vulkan 只做稳定 ID / IR / epoch /
fault 对照。

要问的合同：可移植包如何在第二种 **已执行** 的后端上还原 representation
而不依赖 GPU 指针？驱动升级实验室谁提供？动态图抓包需要已冻结的发现集，
不能发明节点。

### 2.6 证书合成（对应 12 §8 未决 3）

现有断点：种子拓扑证书盖住本次走图见证。跨间接调用 / Task child 的合成没做。

要问的合同：子证书如何合并且保持 sound over-approximation，而不爆炸成 Universe？

## 3. 本机明确不会做

- 不实现 UMD / KMD / firmware。
- 不绕过 Metal、WDDM、Linux DRM 或厂商驱动。
- 不把本笔记当成 Phase E 或驱动里程碑的入口。
- 不把未跑的 Vulkan 填进执行证据。

## 4. 复访条件

出现下列之一再写 v2，而不是现在开写驱动：

- 可执行的 Vulkan 真机，或 discrete-GPU Metal 上出现与 proxy 不同的驻留计数；
- 一次可执行的 Vulkan DGC 多管线 select（可标 `DevicePass`）；Metal ICB GPU-encode 已落地，不单独构成 v2；
- 一份不经 host 读回、仍能盖住见证的设备侧发现集；
- 预授权信封在硬件上可观察，而不是第二次 submit。
