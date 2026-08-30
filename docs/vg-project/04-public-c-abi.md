# 04 Public C ABI

本文件规定第一版 VG 公共 ABI 的形状。示例不是已冻结头文件；Agent 实现时必须维护 `include/vg/vg.h` 与本规范同步，并对任何 ABI 变化增加测试。

## 1. 设计目标

- C11 可包含，C++、Rust、Swift、Python FFI 可绑定；
- ABI 稳定、结构体可扩展、枚举大小明确；
- 不暴露 STL、异常、Objective-C、Metal/Vulkan 类型；
- 不把内存地址、backend handle 和 VG object handle 混为一谈；
- 所有失败可程序化判断；
- 支持静态链接与运行时 function table；
- hot path 可以批量提交，不要求逐对象虚调用。

## 2. 基础约定

```c
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
# define VG_CALL __cdecl
# if defined(VG_BUILD_SHARED)
#  define VG_API __declspec(dllexport)
# else
#  define VG_API
# endif
#else
# define VG_CALL
# define VG_API __attribute__((visibility("default")))
#endif

typedef uint32_t VgBool32;
typedef uint64_t VgDeviceSize;
typedef uint64_t VgGpuAddress;
typedef uint64_t VgStableId;
```

所有 public struct 的整数使用固定宽度类型。`size_t` 只用于当前进程 host 字节数组长度，不进入 capture/on-GPU ABI。布尔值使用 `VgBool32`。公共 ABI 不使用 bitfield、C enum 作为存储字段或编译器相关 packing。

## 3. Handle

```c
typedef struct VgRuntime_T* VgRuntime;
typedef struct VgAdapter_T* VgAdapter;
typedef struct VgDevice_T* VgDevice;
typedef struct VgAddressDomain_T* VgAddressDomain;
typedef struct VgArena_T* VgArena;
typedef struct VgAllocation_T* VgAllocation;
typedef struct VgCodeObject_T* VgCodeObject;
typedef struct VgNode_T* VgNode;
typedef struct VgTimeline_T* VgTimeline;
typedef struct VgTaskGraphBuilder_T* VgTaskGraphBuilder;
typedef struct VgTaskGraph_T* VgTaskGraph;
typedef struct VgExecutionEnvelope_T* VgExecutionEnvelope;
typedef struct VgSubmission_T* VgSubmission;
typedef struct VgCapture_T* VgCapture;
```

Host ABI 使用 opaque pointer handle 便于 debugger；实现不得解引用用户伪造值。GPU 数据结构不嵌入这些 host handle，而使用规范化的 `VgNodeRef`、`VgFacetRef` 等 32/64 位 capability token。

## 4. 结果与错误

```c
typedef int32_t VgResult;

#define VG_SUCCESS                         ((VgResult)0)
#define VG_INCOMPLETE                      ((VgResult)1)
#define VG_NOT_READY                       ((VgResult)2)
#define VG_ERROR_INVALID_ARGUMENT          ((VgResult)-1)
#define VG_ERROR_INVALID_STATE             ((VgResult)-2)
#define VG_ERROR_UNSUPPORTED               ((VgResult)-3)
#define VG_ERROR_OUT_OF_HOST_MEMORY        ((VgResult)-4)
#define VG_ERROR_OUT_OF_DEVICE_MEMORY      ((VgResult)-5)
#define VG_ERROR_STALE_HANDLE              ((VgResult)-6)
#define VG_ERROR_CONTRACT_VIOLATION        ((VgResult)-7)
#define VG_ERROR_ACCESS_CERTIFICATE        ((VgResult)-8)
#define VG_ERROR_DEVICE_LOST               ((VgResult)-9)
#define VG_ERROR_TIMEOUT                   ((VgResult)-10)
```

约定：非负是成功或状态，负数是失败；函数失败时不修改输出 handle；线程局部 extended diagnostic 可通过 `vgGetLastDiagnostic` 获取，但 correctness 不能依赖文本。异步错误通过 Timeline/Event fault record 报告。

## 5. 可扩展结构体

每个输入结构体前缀包含：

```c
typedef struct VgStructHeader {
    uint32_t type;
    uint32_t size;
    const void* next;
} VgStructHeader;
```

- `type` 是固定 `uint32_t` 常量；
- `size` 允许旧 runtime 读取已知前缀；
- `next` 是只读扩展链，必须无环；
- 未知 optional extension 被忽略并记录；未知 required extension 返回 unsupported；
- 输出结构由调用方设置 `type/size`，runtime 只写已知且落在 size 内字段。

不允许通过在普通 struct 尾部盲加字段破坏 ABI。

## 6. Runtime 与 function table

```c
#define VG_API_VERSION_1_0 0x00010000u

typedef void* (VG_CALL *VgAllocateFn)(void* user, size_t size, size_t alignment);
typedef void  (VG_CALL *VgFreeFn)(void* user, void* memory);
typedef void  (VG_CALL *VgLogFn)(void* user, uint32_t severity,
                                 uint32_t category, const char* message);

typedef struct VgRuntimeDesc {
    VgStructHeader header;
    void* user;
    VgAllocateFn allocate;
    VgFreeFn free;
    VgLogFn log;
    uint64_t flags;
    uint32_t validation_profile;
    uint32_t reserved;
} VgRuntimeDesc;

typedef struct VgApi {
    uint32_t version;
    uint32_t size;
    VgResult (VG_CALL *createRuntime)(const VgRuntimeDesc*, VgRuntime*);
    void (VG_CALL *destroyRuntime)(VgRuntime);
    /* Versioned function pointers continue here. */
} VgApi;

VG_API VgResult VG_CALL vgGetApi(uint32_t requested_version, VgApi* out_api);
```

`VG_API_VERSION_1_0` 是本节示例的最小骨架版本。实际 header/API 的版本矩阵
如下（以 `include/vg/vg_version.h` 为准）：

| 版本 | 记录 | ABI 变化 |
|---|---|---|
| v1.0 | ADR-042 前的最小 ABI | 初始 function table |
| v1.1 | ADR-044 | 完整对象链和 `getSubmissionLoweringReport` |
| v1.2 | ADR-045 | `getSubmissionExecutionResult` |
| v1.3 | ADR-048 | `acquireFacet` 与 raster ABI |
| v1.4 | ADR-049 | `VgTaskRecordV2` / `taskGraphAppendV2` |
| v1.5 | ADR-050 | index format/header capability；不增长 table |
| v1.6 | ADR-051 | `writeAllocation` / `readAllocation` |
| v1.7 | ADR-052 | SceneRoot contract；不增长 table |

每一版的 function table 只在尾部追加函数指针，`vgGetApi` 按请求版本对应的
`size` 分档 `memcpy`，旧版本调用方对新 runtime 保持字节兼容。ADR-053 的
device-scoped NodeRef/multi-CodeObject 语义修复复用既有 `VgNodeRef`、builder
descriptor 和 table，因此不发布 v1.8；它不改变任何 v1.0–v1.7 的 layout 或
function-table boundary。

导出 `vgGetApi` 是最小 loader ABI。也可为易用性导出同名函数，但规范测试以 function table 为准。Runtime allocator 必须成对使用；callback 不能抛异常跨越 C ABI。

## 7. Adapter 与能力查询

能力不应成为巨型固定 struct。基础 identity 固定，小项按 property chain 查询：

```c
typedef struct VgAdapterInfo {
    VgStructHeader header;
    uint8_t stable_uuid[16];
    uint32_t backend_kind;
    uint32_t adapter_class;
    char name[128];
    char driver[128];
} VgAdapterInfo;

typedef struct VgCapabilityQuery {
    VgStructHeader header;
    uint32_t capability;
    uint32_t reserved;
    void* output;
    uint32_t output_size;
    uint32_t written_size;
} VgCapabilityQuery;
```

必须查询的关键能力：address width/alignment、host coherency、memory heaps、sparse/fault model、facet limits、Task tier、timeline scope、subgroup、raster/ray/tensor domain、external memory、capture determinism 与 supported IR versions。

## 8. 地址、Region 与 schema

裸 `VgGpuAddress` 只作为同一 AddressDomain 内的编码值。Host API 同时携带 allocation/provenance；用户不可用整数算术凭空扩大授权。

```c
typedef struct VgRegionDesc {
    VgStructHeader header;
    VgAddressDomain domain;
    VgAllocation allocation;
    uint64_t byte_offset;
    uint64_t byte_size;
    VgStableId schema_id;
    uint64_t shape[4];
    uint64_t strides[4];
    uint64_t access_mask;
    uint32_t layout_class;
    uint32_t rank;
} VgRegionDesc;
```

API 不创建 `Buffer` 或 `Texture` 生命周期对象。`VgRegionDesc` 是逻辑视图描述，sample/attachment 所需 backend facet 通过 Node/representation compilation 获得版本化 `VgFacetRef`。

## 9. Task ABI

第一版 canonical host layout：

```c
typedef struct VgNodeRef {
    uint32_t index;
    uint32_t generation;
} VgNodeRef;

typedef struct VgExecutionShape {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t flags;
} VgExecutionShape;

typedef struct VgTaskRecord {
    VgNodeRef node;
    uint64_t root;
    uint32_t root_generation;
    VgExecutionShape shape;
    uint32_t contract_index;
    uint32_t payload_size;
    uint64_t payload_or_offset;
} VgTaskRecord;
```

`root_generation`（ADR-044，v1.1）补上了 `core::TaskRecord` 本就有的
`root_allocation`/`root_generation` 拆分身份——本节最初的示例 `root` 是裸
`uint64_t`，没有位置放 generation；`root_generation` 是本文档 §17 明确授权
的“示例字段可随 ADR 调整”的一次实际使用，不是未披露的漂移。

具体 on-GPU ABI 由 schema 编译器产生，并有显式 version/hash。CPU 和 GPU 生成者必须使用同一 generated header。Task 发布协议通常为“写 payload -> release fence -> 原子发布状态/队尾”；consumer acquire 后不得观察半写记录。

### 9.1 v1.3 补充：Raster 接入公共 C ABI（ADR-046 / ADR-047 / ADR-048）

F2/F3（ADR-046、ADR-047）先让 rasterization 成为 `core::TaskRecord`/
`ExecutionPlan` 的一种内部形状，但故意没有触碰公共 C ABI——ADR-046
Consequences 明确写道“a public raster ABI entry point is deferred to a
later F milestone”。F3.5（ADR-048）把这个此前故意留空的缺口补上：
`VgTaskRecord` 在尾部追加一组新字段，新增 `VgCanonicalViewDesc`/
`VgRasterFacetPair` 两个 struct，以及一个新的 `acquireFacet` 入口点。这是
第二次对 `VgTaskRecord` 使用 §17 的“示例字段可随 ADR 调整”授权（第一次是
上面 §9 的 `root_generation`），**披露、有引用**，因此不属于 §5 规则
（“不允许通过在普通 struct 尾部盲加字段破坏 ABI”）禁止的“盲加”。

`VgTaskRecord` v1.3 追加的字段（紧跟在 `payload_or_offset` 之后）：

```c
typedef struct VgTaskRecord {
    VgNodeRef node;
    uint64_t root;
    uint32_t root_generation;
    VgExecutionShape shape;
    uint32_t contract_index;
    uint32_t payload_size;
    uint64_t payload_or_offset;
    /* ---- v1.3 追加（F2/ADR-046、F3.5/ADR-048） ---- */
    uint32_t kind;                     /* VG_TASK_KIND_* */
    uint32_t topology;                 /* VG_TOPOLOGY_* */
    VgRasterFacetPair raster_facets;
    VgFacetRef vertex_buffer_ref;
    VgFacetRef index_buffer_ref;
    uint32_t index_count;
    uint32_t raster_filter;            /* VG_FILTER_* */
    uint32_t raster_wrap;              /* VG_WRAP_* */
    float raster_tint[4];
} VgTaskRecord;

typedef struct VgRasterFacetPair {
    VgFacetRef source;
    VgFacetRef target;
} VgRasterFacetPair;
```

**已披露的 recompile hazard**：与本文件其余每个 struct 不同，
`VgTaskRecord` 没有 per-element `VgStructHeader`，`taskGraphAppend` 把它当
裸数组传递，没有像 `VgApi` 那样的按次 `size` 协商。追加字段改变了
`sizeof(VgTaskRecord)`；用旧（更小）header 编译、链接到新 `libvg` 的调用方
必须重新编译，否则 `taskGraphAppend` 的数组下标会按库里编译进去的更大
`sizeof` 计算，读出调用方自己数组边界之外。

**已披露的 zero-init 默认值不匹配**：零初始化的 `VgTaskRecord` 能正确解出
`kind = VG_TASK_KIND_COMPUTE`、`topology = VG_TOPOLOGY_TRIANGLE_LIST`、
`raster_wrap = VG_WRAP_CLAMP`（均为序数 0，与内部引擎真实默认值一致），但
**不能**正确解出 `raster_filter`（零解出 `VG_FILTER_NEAREST`，真实默认是
`VG_FILTER_BILINEAR`）或 `raster_tint`（零解出 `{0,0,0,0}`，真实默认是不
透明白色 `{1,1,1,1}`）。字段一律原样拷贝，不做 default-substitution；提交
`VG_TASK_KIND_RASTER` task 的调用方必须显式设置这两个字段。

`acquireFacet` 的输入（一次性调用，非热路径数组元素，因此保留标准的
`VgStructHeader` 可扩展 struct 约定）：

```c
typedef struct VgFacetRef {
    uint32_t index;
    uint32_t generation;
} VgFacetRef;

typedef struct VgCanonicalViewDesc {
    VgStructHeader header;
    uint64_t allocation;
    uint32_t allocation_generation;
    uint32_t format;            /* VG_PIXEL_FORMAT_* */
    uint32_t dimension;         /* VG_VIEW_DIMENSION_* */
    uint32_t width;
    uint32_t height;
    uint32_t array_layers;
    uint32_t mip_levels;
    uint32_t swizzle_red;       /* VG_SWIZZLE_* */
    uint32_t swizzle_green;
    uint32_t swizzle_blue;
    uint32_t swizzle_alpha;
} VgCanonicalViewDesc;

VgResult vgAcquireFacet(VgDevice device, VgArena arena,
                        const VgCanonicalViewDesc* view,
                        uint32_t facet_kind, VgFacetRef* out_facet);
```

`VgCanonicalViewDesc` 与内部 `core::CanonicalView` 逐字段镜像；
`allocation`/`allocation_generation` 是裸 id/generation pair（不是
`VgAllocation` handle），与 `core::CanonicalView` 的真实字段类型一致，通过
既有 `getAllocationRef(VgAllocation, uint64_t*, uint32_t*)`（v1.1）获得。
`acquireFacet` 是 `core::FacetPool::acquire` 的公共入口，是 v1.3 唯一的新
函数表成员，紧跟在 v1.2 的 `getSubmissionExecutionResult` 之后
（append-only、`size` 分档协商，ADR-044/ADR-045 既有纪律的第三次延伸）。
后端无关：三个 backend 共享同一个 `DeviceHal::facet_pool()`，不像 F3
（受限 MSL 导入）那样按 backend 分叉实现。

v1.3 新增的枚举值块（与 `core::TaskKind`/`core::Topology`/
`core::FilterMode`/`core::WrapMode`/`core::PixelFormat`/
`core::ViewDimension`/`core::Swizzle` 按序数对应）：

```c
VG_STRUCTURE_CANONICAL_VIEW_DESC = 14u,
VG_TASK_KIND_COMPUTE = 0u,     VG_TASK_KIND_RASTER = 1u,
VG_TOPOLOGY_TRIANGLE_LIST = 0u,
VG_FILTER_NEAREST = 0u,        VG_FILTER_BILINEAR = 1u,
VG_WRAP_CLAMP = 0u,            VG_WRAP_REPEAT = 1u,
VG_PIXEL_FORMAT_RGBA8_UNORM = 0u, VG_PIXEL_FORMAT_R32_FLOAT = 1u,
VG_VIEW_DIMENSION_TEXTURE_2D = 0u, VG_VIEW_DIMENSION_TEXTURE_2D_ARRAY = 1u,
VG_SWIZZLE_RED = 0u,  VG_SWIZZLE_GREEN = 1u, VG_SWIZZLE_BLUE = 2u,
VG_SWIZZLE_ALPHA = 3u, VG_SWIZZLE_ZERO = 4u,  VG_SWIZZLE_ONE = 5u,
```

范围外（本节不涉及，见 ADR-048 Consequences）：混合 compute+raster 一次
submission（ADR-047 Decision #6 仍然拒绝）；indexed raster 绘制
（`index_count > 0` 仍在 `compile()` 时被拒绝，F5 才实现）；depth（F4）；
per-shader 可声明绑定。

## 10. Builder 与 seal

可变构建与不可变执行分开：

```c
VgResult vgCreateTaskGraphBuilder(VgDevice, const VgTaskGraphBuilderDesc*,
                                  VgTaskGraphBuilder*);
VgResult vgTaskGraphAppend(VgTaskGraphBuilder, const VgTaskRecord* tasks,
                           uint32_t task_count, VgTaskId* out_ids);
VgResult vgTaskGraphAddDependency(VgTaskGraphBuilder, VgTaskId before,
                                  VgTaskId after);
VgResult vgSealTaskGraph(VgTaskGraphBuilder, const VgSealDesc*, VgTaskGraph*);
```

Builder 不能 submit；sealed graph 不能修改。重复执行通过不同 Envelope/root data，而不是修改已发布 Task。`out_ids`（ADR-044，v1.1）按 append 顺序把每个 task 分配到的 `VgTaskId` 写回调用方数组，供后续 `vgTaskGraphAddDependency` 引用——`VgTaskId` 只在它来源的那一个 `VgTaskGraphBuilder`/`VgTaskGraph` 内有效，不是跨 graph 的 handle。

ADR-053 supersedes ADR-044 的单 `CodeObject` narrowing：builder descriptor
的 `code_object` 是可为 null 的、同一 Device 兼容性提示，而不是 graph 的
program owner。每个 Task 的完整 `VgNodeRef {index, generation}` 在 Device 的
NodeTable 中解析，故一个 graph 可引用同一 Device 上来自多个 CodeObject 的
Node。该 descriptor 的既有布局和 function-table 协商均未改变；Envelope 若
提供 Node allow-list，必须以完整 index+generation 授权，不能仅以 index/class
授权。当前只有 Reference 的 canonical-compute 路径支持此 multi-CodeObject
组合；Metal/Vulkan 必须返回 `VG_ERROR_UNSUPPORTED`，且 ADR-047/ADR-052 的
mixed compute+raster 限制仍然适用。

## 11. ExecutionEnvelope 与提交

```c
typedef struct VgTimelinePoint {
    VgTimeline timeline;
    uint64_t value;
} VgTimelinePoint;

typedef struct VgSubmitDesc {
    VgStructHeader header;
    VgTaskGraph graph;
    VgExecutionEnvelope envelope;
    const VgTimelinePoint* waits;
    uint32_t wait_count;
    const VgTimelinePoint* signals;
    uint32_t signal_count;
    uint64_t flags;
} VgSubmitDesc;

VgResult vgSubmit(VgDevice device, const VgSubmitDesc* submit,
                  VgSubmission* out_submission);
```

Timeline value 单调递增；重复或倒退 signal 是错误。Host wait 必须带 timeout。GPU Task 不直接写 host handle；只可写 envelope 授权的 local event/counter。跨 queue、跨进程与 display ownership 由控制平面提交完成。

## 12. AccessCertificate ABI

证书是可序列化的声明，不是 callback：

```c
typedef struct VgAccessRange {
    VgAllocation allocation;
    uint64_t offset;
    uint64_t size;
    uint64_t access_mask;
    uint32_t representation_epoch;
    uint32_t reserved;
} VgAccessRange;

typedef struct VgAccessCertificateDesc {
    VgStructHeader header;
    uint32_t mode;
    uint32_t range_count;
    const VgAccessRange* ranges;
    VgTaskGraph discovery_graph;
    uint64_t universe_budget;
} VgAccessCertificateDesc;
```

mode 至少有 `CERTIFIED_PINNED`、`DISCOVER_THEN_LEASE`、`FAULT_MANAGED`、`UNIVERSE` 和 `SOFTWARE_PAGED`。后端不支持可恢复 fault 时不得接受 `FAULT_MANAGED` 并假装可恢复。

## 13. Diagnostics

结构化诊断包含：result、subsystem、object stable ID、Task/Node ID、source span、expected/actual effect、backend code、lowering class 与短文本。文本不是稳定 API。开发工具可查询：

- validation messages；
- `LoweringReport`；
- AccessWitness expected/actual diff；
- timeline/fault/poison record；
- capture stable object graph。

## 14. 线程安全表

| API 类别 | 并发规则 |
|---|---|
| 查询与 immutable object | fully concurrent |
| Runtime/Device creation | concurrent，allocator 必须安全 |
| builder mutation | external synchronization，除非接口标记 concurrent chunk |
| Arena allocation/free | internal synchronization |
| host map write | application controls data race |
| submit/timeline query | fully concurrent |
| destroy | 不得与同一 host handle 的其他调用并发；in-flight GPU lifetime 自动延迟 |

## 15. C++ wrapper 规则

C++ wrapper 是独立 header-only 或小库：提供 RAII、`span`、typed `Region<T>`、expected/result、builder；不得改变 ownership、自动插入 host wait、隐式提交或吞掉 lowering warning。C ABI conformance 必须能在不使用 wrapper 时完整运行。

## 16. ABI 测试

每个发布必须运行：

- C11/C++17/Clang/GCC 编译包含测试；
- `sizeof/alignof/offsetof` golden；
- 旧 header + 新 runtime、新 header + 旧 runtime compatibility；
- unknown optional/required extension；
- invalid/stale/fuzzed handles；
- null、zero-count、overflow、unaligned input；
- callback reentrancy；
- shared library symbol allowlist；
- capture schema version round-trip。

## 17. 最小应用的完整调用逻辑

以下代码是首版 API 骨架的**历史性语义伪代码**，用于展示对象何时属于控制平面、普通数据何时进入 Arena，以及提交前如何 seal；它不是可直接编译的当前 header 示例。当前可编译的 F6/v1.7 SceneRoot 公共 C 路径以
[`tests/api/vg_f6_scene_root.c`](../../tests/api/vg_f6_scene_root.c) 为准：它使用
`VgSchema_SceneRootRaster`、`VG_SCHEMA_SCENEROOTRASTER_CONTRACT_NAME`、
`writeAllocation` 和既有 `VgTaskRecordV2.root/root_generation`。这避免把未实现的
helper 当成 ABI 合同。

```c
#include <vg/vg.h>

int main(void) {
    VgApi api = { .version = VG_API_VERSION_1_0, .size = sizeof(VgApi) };
    if (vgGetApi(VG_API_VERSION_1_0, &api) < 0) return 1;

    VgRuntimeDesc runtime_desc = VG_INIT_STRUCT(VG_STRUCTURE_RUNTIME_DESC);
    runtime_desc.validation_profile = VG_VALIDATION_CHECKED_NATIVE;

    VgRuntime runtime = NULL;
    VG_CHECK(api.createRuntime(&runtime_desc, &runtime));

    /* Enumeration never silently chooses a GPU. */
    uint32_t adapter_count = 0;
    VG_CHECK(api.enumerateAdapters(runtime, NULL, &adapter_count, NULL));
    VgAdapterInfo infos[VG_EXAMPLE_MAX_ADAPTERS];
    adapter_count = min_u32(adapter_count, VG_EXAMPLE_MAX_ADAPTERS);
    VG_CHECK(api.enumerateAdapters(runtime, NULL, &adapter_count, infos));

    uint32_t selected = choose_adapter(infos, adapter_count,
                                       VG_REQUIRE_COMPUTE |
                                       VG_REQUIRE_TYPED_ADDRESS);
    VgAdapter adapter = NULL;
    VG_CHECK(api.openAdapter(runtime, &infos[selected].stable_uuid, &adapter));

    VgDeviceDesc device_desc = VG_INIT_STRUCT(VG_STRUCTURE_DEVICE_DESC);
    device_desc.required_profile = VG_PROFILE_PORTABLE_COMPUTE_1;
    VgDevice device = NULL;
    VG_CHECK(api.createDevice(adapter, &device_desc, &device));

    VgAddressDomainDesc domain_desc =
        VG_INIT_STRUCT(VG_STRUCTURE_ADDRESS_DOMAIN_DESC);
    domain_desc.kind = VG_ADDRESS_DOMAIN_DEVICE_STABLE;
    VgAddressDomain domain = NULL;
    VG_CHECK(api.createAddressDomain(device, &domain_desc, &domain));

    VgArenaDesc arena_desc = VG_INIT_STRUCT(VG_STRUCTURE_ARENA_DESC);
    arena_desc.reserve_size = 64u * 1024u * 1024u;
    arena_desc.memory_policy = VG_MEMORY_DEVICE_PREFERRED;
    VgArena arena = NULL;
    VG_CHECK(api.createArena(domain, &arena_desc, &arena));

    VgAllocation root_alloc = NULL;
    VgAllocation root_data_alloc = NULL;
    VG_CHECK(api.arenaAllocate(arena, sizeof(SceneRoot),
                               _Alignof(SceneRoot), &root_alloc));
    VG_CHECK(api.arenaAllocate(arena, input_bytes, 16, &root_data_alloc));

    /* Current F6 code writes generated SceneRoot bytes explicitly with
     * writeAllocation; there is no vgMakeSceneRoot/upload_scene_root ABI. */

    VgCodeObject code = NULL;
    VG_CHECK(api.loadCodeObject(device, code_package_bytes,
                                code_package_size, &code));

    VgNodeDesc node_desc = VG_INIT_STRUCT(VG_STRUCTURE_NODE_DESC);
    node_desc.code = code;
    node_desc.entry = "process_scene";
    node_desc.root_schema = VG_SCHEMA_ID_SCENE_ROOT;
    VgNode node = NULL;
    VG_CHECK(api.createNode(device, &node_desc, &node));

    VgTaskGraphBuilder builder = NULL;
    VG_CHECK(api.createTaskGraphBuilder(device, NULL, &builder));
    VgTaskRecord task = vgMakeTask(api.getNodeRef(node),
                                   api.getAllocationAddress(root_alloc),
                                   (VgExecutionShape){group_count, 1, 1, 0});
    VG_CHECK(api.taskGraphAppend(builder, &task, 1));

    VgTaskGraph graph = NULL;
    VG_CHECK(api.sealTaskGraph(builder, NULL, &graph));
    api.destroyTaskGraphBuilder(builder);

    VgAccessRange ranges[] = {
        vgReadRange(root_alloc),
        vgReadWriteRange(root_data_alloc)
    };
    VgAccessCertificateDesc cert =
        VG_INIT_STRUCT(VG_STRUCTURE_ACCESS_CERTIFICATE_DESC);
    cert.mode = VG_RESIDENCY_CERTIFIED_PINNED;
    cert.ranges = ranges;
    cert.range_count = 2;

    VgExecutionEnvelopeDesc envelope_desc =
        VG_INIT_STRUCT(VG_STRUCTURE_EXECUTION_ENVELOPE_DESC);
    envelope_desc.allowed_nodes = &node;
    envelope_desc.allowed_node_count = 1;
    envelope_desc.certificate = &cert;
    envelope_desc.task_quota = 1;

    VgExecutionEnvelope envelope = NULL;
    VG_CHECK(api.createExecutionEnvelope(device, &envelope_desc, &envelope));

    VgTimeline timeline = NULL;
    VG_CHECK(api.createTimeline(device, 0, &timeline));
    VgTimelinePoint done = { timeline, 1 };
    VgSubmitDesc submit = VG_INIT_STRUCT(VG_STRUCTURE_SUBMIT_DESC);
    submit.graph = graph;
    submit.envelope = envelope;
    submit.signals = &done;
    submit.signal_count = 1;

    VgSubmission submission = NULL;
    VG_CHECK(api.submit(device, &submit, &submission));
    VG_CHECK(api.waitTimeline(timeline, 1, VG_TIMEOUT_INFINITE));

    VgLoweringReport report = VG_INIT_STRUCT(VG_STRUCTURE_LOWERING_REPORT);
    VG_CHECK(api.getSubmissionLoweringReport(submission, &report));
    inspect_no_hidden_host_round_trip(&report);

    /* Host handle release may precede physical retirement; epochs protect GPU use. */
    api.destroySubmission(submission);
    api.destroyTimeline(timeline);
    api.destroyExecutionEnvelope(envelope);
    api.destroyTaskGraph(graph);
    api.destroyNode(node);
    api.destroyCodeObject(code);
    api.destroyArena(arena);
    api.destroyAddressDomain(domain);
    api.destroyDevice(device);
    api.closeAdapter(adapter);
    api.destroyRuntime(runtime);
    return 0;
}
```

调用背后的关键逻辑：

1. `openAdapter/createDevice` 建立 OS/driver authority；普通 GPU 数据不能替代它。
2. Arena allocation 产生 backing 与地址身份；generated schema 产生合法 typed references。
3. CodeObject 的编译结果带 effect/contract，Node 把入口注册成 capability。
4. Builder 只收集可变 Task；seal 后 graph 不可变。
5. Envelope 将 Node 授权、certificate、epoch、quota 和 timeline 组合成可审核提交。
6. Core 完成 Stage 0-5 验证，DeviceHAL 才看见 immutable plan。
7. Adapter lowering 到 Metal/Vulkan，提交后产生 Timeline、fault 和 LoweringReport。
8. Host destroy 不强迫同步；实际回收由 in-flight epoch retirement 决定。

首版实现可以暂时缩小函数集合，但不能通过隐式全局 Device、隐式 GPU 选择、隐式同步或 backend handle 泄漏来缩短样例。
