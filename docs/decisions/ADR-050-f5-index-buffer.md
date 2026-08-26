# ADR-050: F5 Index Buffer（V2 task contract）

## Context

F4 已冻结 `VgTaskRecordV2` 的 raw-array ABI；再扩展其尾部或再增加
`taskGraphAppendV3` 都会扩大 ABI 整合与维护负担。现有 V2 已经包含
`index_buffer_ref` 与 `index_count`，缺少的只是 index 元素格式和真实后端路径。

## Decision

F5 发布 API/header v1.5，但不增长 `VgApi` 表，也不新建 append 函数。
新调用方请求 v1.5 并沿用 `VgTaskRecordV2` / `taskGraphAppendV2`。

`R16Uint` 与 `R32Uint` 是 CanonicalView format；仅能取得 Address facet。
index Address facet 的 format 决定 index 元素类型，因而不需要向 V2
追加 type 字段。F5 仅支持从 backing allocation offset zero 读取、TriangleList、
u16/u32、非 indexed 与 indexed 两条路径；不包括 base vertex、first index、
restart、instancing 或 VertexDescriptor。

V1/V2 的 ABI wrappers 只负责解码各自的固定记录布局，统一交给同一内部
normalization/validation/append/diagnostic/task-id 路径。旧 ABI 符号仍保留，
但不保留独立业务实现。

Reference 解码并验证 index bytes 后使用索引展开的顶点序列运行既有 raster/depth
oracle。Metal 直接绑定同一 Address buffer，并用 `drawIndexedPrimitives`；index
format 是 draw-call state，绝不进入 PSO/depth-state key。Vulkan 仍为
compile-review-only，并继续报告 raster task unsupported。

## Validation

index_count 必须非零且为 3 的倍数；index facet 必须为 Address/R16Uint/R32Uint；
`index_count * stride` 做溢出检查，backing allocation 必须足够长，且每一个
index 小于从 vertex bytes 推导出的 vertex count。错误必须诊断而非默认 u16 或
静默降级为 non-indexed。

Reference 的验收同时运行 u16 与 u32 的真实 indexed oracle，并覆盖非三倍
count；Metal `dev-metal` 使用四顶点和 `{0,1,2,2,1,3}` 分别以 u16/u32
执行 `drawIndexedPrimitives`，对整张颜色图与 Reference 差分。深度附件的
全图 readback 继续与同一任务图的 Reference oracle 对照，确保 indexed
变更不回退 F4 深度路径。
