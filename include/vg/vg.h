#ifndef VG_VG_H_
#define VG_VG_H_

#include <stddef.h>
#include <stdint.h>
#include "vg/vg_version.h"

#if defined(_WIN32)
#define VG_CALL __cdecl
#if defined(VG_BUILD_SHARED)
#define VG_API __declspec(dllexport)
#else
#define VG_API
#endif
#else
#define VG_CALL
#if defined(VG_BUILDING_LIBRARY)
#define VG_API __attribute__((visibility("default")))
#else
#define VG_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t VgBool32;
typedef uint64_t VgDeviceSize;
typedef uint64_t VgGpuAddress;
typedef uint64_t VgStableId;
typedef int32_t VgResult;

#define VG_FALSE ((VgBool32)0)
#define VG_TRUE ((VgBool32)1)
#define VG_SUCCESS ((VgResult)0)
#define VG_INCOMPLETE ((VgResult)1)
#define VG_NOT_READY ((VgResult)2)
#define VG_ERROR_INVALID_ARGUMENT ((VgResult)-1)
#define VG_ERROR_INVALID_STATE ((VgResult)-2)
#define VG_ERROR_UNSUPPORTED ((VgResult)-3)
#define VG_ERROR_OUT_OF_HOST_MEMORY ((VgResult)-4)
#define VG_ERROR_OUT_OF_DEVICE_MEMORY ((VgResult)-5)
#define VG_ERROR_STALE_HANDLE ((VgResult)-6)
#define VG_ERROR_CONTRACT_VIOLATION ((VgResult)-7)
#define VG_ERROR_ACCESS_CERTIFICATE ((VgResult)-8)
#define VG_ERROR_DEVICE_LOST ((VgResult)-9)
#define VG_ERROR_TIMEOUT ((VgResult)-10)

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

/* Thread-safety contract for these handles (ADR-044 "Concurrency"): distinct
 * handles of the same type may be freely created/destroyed/validated
 * concurrently. A caller must never call the destroy function for a handle
 * concurrently with any other API call that takes that same handle (or one
 * reachable through it) as an argument -- see ADR-044's Concurrency
 * subsection for the full contract. */

/* v1.1 (ADR-043 Decision #2 / ADR-044): a task's position inside one
 * VgTaskGraphBuilder, assigned sequentially by taskGraphAppend in append
 * order. Scoped to the builder/graph it came from -- it is not a handle and
 * is never valid against a different graph. */
typedef uint32_t VgTaskId;

/* Capability token, not a host handle: GPU data structures embed these
 * index+generation pairs instead of an opaque handle or a backend texture
 * pointer. A facet is obtained from representation compilation and is
 * versioned -- a token whose slot generation or RepresentationEpoch has
 * moved on is stale and must be rejected, not silently resolved. */
typedef struct VgFacetRef {
    uint32_t index;
    uint32_t generation;
} VgFacetRef;

typedef struct VgStructHeader {
    uint32_t type;
    uint32_t size;
    const void* next;
} VgStructHeader;

enum {
    VG_STRUCTURE_REQUIRED_BIT = 0x80000000u,
    VG_STRUCTURE_RUNTIME_DESC = 1u,
    VG_STRUCTURE_ADAPTER_INFO = 2u,
    VG_STRUCTURE_REGION_DESC = 3u,
    VG_FACET_KIND_ADDRESS = 0u,
    VG_FACET_KIND_SAMPLE = 1u,
    VG_FACET_KIND_STORAGE = 2u,
    VG_FACET_KIND_ATTACHMENT = 3u,
    VG_FACET_KIND_TRANSFER = 4u,
    /* Layout/representation traits, not resource lifetimes: the same Region
     * may be interpreted through any of these without becoming a different
     * object. */
    VG_LAYOUT_LINEAR = 0u,
    VG_LAYOUT_TILED = 1u,
    VG_LAYOUT_SAMPLE_OPTIMAL = 2u,
    VG_LAYOUT_STORAGE_OPTIMAL = 3u,
    VG_LAYOUT_ATTACHMENT = 4u,
    VG_LAYOUT_TENSOR = 5u,
    VG_LAYOUT_ACCEL = 6u,
    VG_LAYOUT_VIDEO = 7u,
    /* Access classes feed effect inference and backend lowering. They are a
     * declaration of intent, not a public old/new usage state machine: nothing
     * transitions a Region from one of these to another. */
    VG_ACCESS_READ = 1u << 0,
    VG_ACCESS_WRITE = 1u << 1,
    VG_ACCESS_ATOMIC = 1u << 2,
    VG_ACCESS_SAMPLE_READ = 1u << 3,
    VG_ACCESS_ATTACHMENT_WRITE = 1u << 4,
    VG_ACCESS_PUBLISH = 1u << 5,
    VG_ACCESS_TRANSFER = 1u << 6,
    VG_ACCESS_METADATA = 1u << 7,
    VG_ACCESS_INSTRUCTION = 1u << 8,
    VG_VALIDATION_DISABLED = 0u,
    VG_VALIDATION_REFERENCE_STRICT = 1u,
    VG_VALIDATION_CHECKED_NATIVE = 2u,
    VG_VALIDATION_FAST_NATIVE = 3u,
    VG_VALIDATION_CAPTURE = 4u,
    VG_BACKEND_REFERENCE = 1u,
    VG_BACKEND_METAL = 2u,
    VG_BACKEND_VULKAN = 3u,
    VG_ADAPTER_CLASS_CPU = 1u,
    VG_ADAPTER_CLASS_INTEGRATED_GPU = 2u,
    VG_ADAPTER_CLASS_DISCRETE_GPU = 3u,
    VG_LOG_ERROR = 1u,
    VG_LOG_WARNING = 2u,
    VG_LOG_INFO = 3u,
    /* v1.1 additions (ADR-043 Decision #2 / ADR-044). */
    VG_STRUCTURE_DEVICE_DESC = 4u,
    VG_STRUCTURE_ADDRESS_DOMAIN_DESC = 5u,
    VG_STRUCTURE_ARENA_DESC = 6u,
    VG_STRUCTURE_CODE_OBJECT_DESC = 7u,
    VG_STRUCTURE_NODE_DESC = 8u,
    VG_STRUCTURE_TASK_GRAPH_BUILDER_DESC = 9u,
    VG_STRUCTURE_SEAL_DESC = 10u,
    VG_STRUCTURE_ACCESS_CERTIFICATE_DESC = 11u,
    VG_STRUCTURE_EXECUTION_ENVELOPE_DESC = 12u,
    VG_STRUCTURE_SUBMIT_DESC = 13u,
    /* core::AddressDomain kind values. */
    VG_ADDRESS_DOMAIN_DEVICE_LOCAL = 0u,
    VG_ADDRESS_DOMAIN_HOST_VISIBLE = 1u,
    /* core::AccessCertificateMode, ordinal-matched. */
    VG_ACCESS_CERTIFICATE_MODE_CERTIFIED_PINNED = 0u,
    VG_ACCESS_CERTIFICATE_MODE_UNIVERSE = 1u,
    VG_ACCESS_CERTIFICATE_MODE_DISCOVER_THEN_LEASE = 2u,
    VG_ACCESS_CERTIFICATE_MODE_SOFTWARE_PAGED = 3u,
    VG_ACCESS_CERTIFICATE_MODE_FAULT_MANAGED = 4u,
    /* v1.3 additions (F2/ADR-046, F3.5/ADR-048): raster reaches the public
     * C-ABI. Ordinal-matched to core::TaskKind, core::Topology,
     * core::FilterMode, core::WrapMode, core::PixelFormat,
     * core::ViewDimension and core::Swizzle respectively. */
    VG_STRUCTURE_CANONICAL_VIEW_DESC = 14u,
    VG_TASK_KIND_COMPUTE = 0u,
    VG_TASK_KIND_RASTER = 1u,
    VG_TOPOLOGY_TRIANGLE_LIST = 0u,
    VG_FILTER_NEAREST = 0u,
    VG_FILTER_BILINEAR = 1u,
    VG_WRAP_CLAMP = 0u,
    VG_WRAP_REPEAT = 1u,
    VG_PIXEL_FORMAT_RGBA8_UNORM = 0u,
    VG_PIXEL_FORMAT_R32_FLOAT = 1u,
    VG_VIEW_DIMENSION_TEXTURE_2D = 0u,
    VG_VIEW_DIMENSION_TEXTURE_2D_ARRAY = 1u,
    VG_SWIZZLE_RED = 0u,
    VG_SWIZZLE_GREEN = 1u,
    VG_SWIZZLE_BLUE = 2u,
    VG_SWIZZLE_ALPHA = 3u,
    VG_SWIZZLE_ZERO = 4u,
    VG_SWIZZLE_ONE = 5u
};

typedef void* (VG_CALL *VgAllocateFn)(void* user, size_t size, size_t alignment);
typedef void (VG_CALL *VgFreeFn)(void* user, void* memory);
typedef void (VG_CALL *VgLogFn)(void* user, uint32_t severity, uint32_t category,
                                const char* message);

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

/* The public form of a logical view over an allocation: shape, layout class
 * and access intent, never a Buffer or Texture lifetime object. Sample and
 * attachment use of the same Region is reached through a versioned VgFacetRef
 * obtained from representation compilation, which is why nothing here names a
 * backend resource. No entry point consumes this yet -- the first version of
 * the API deliberately ships a reduced function set -- but its layout is
 * pinned by the ABI golden test so it cannot drift from the runtime's internal
 * canonical view. */
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

typedef struct VgAdapterInfo {
    VgStructHeader header;
    uint8_t stable_uuid[16];
    uint32_t backend_kind;
    uint32_t adapter_class;
    char name[128];
    char driver[128];
} VgAdapterInfo;

/* ---- v1.1 additions (ADR-043 Decision #2 / ADR-044) ------------------- */

typedef struct VgDeviceDesc {
    VgStructHeader header;
    uint32_t validation_profile;
    uint32_t reserved;
} VgDeviceDesc;

typedef struct VgAddressDomainDesc {
    VgStructHeader header;
    uint32_t kind;
    uint32_t reserved;
} VgAddressDomainDesc;

typedef struct VgArenaDesc {
    VgStructHeader header;
    VgAddressDomain domain;
} VgArenaDesc;

typedef struct VgCodeObjectDesc {
    VgStructHeader header;
    const void* bytes;
    uint64_t byte_size;
    const char* format_tag;
} VgCodeObjectDesc;

typedef struct VgNodeDesc {
    VgStructHeader header;
    const char* entry_name;
} VgNodeDesc;

/* Capability token identifying one Node entry inside a VgCodeObject --
 * index+generation, the same staleness-checked shape as VgFacetRef, obtained
 * via getNodeRef and embedded in VgTaskRecord.node. */
typedef struct VgNodeRef {
    uint32_t index;
    uint32_t generation;
} VgNodeRef;

typedef struct VgTaskGraphBuilderDesc {
    VgStructHeader header;
    /* v1.1 narrowing (ADR-044): every task appended to the resulting graph
     * runs against this one CodeObject's module. Multi-module task graphs
     * (one per Effect DAG pass) are deferred past F1. */
    VgCodeObject code_object;
    uint32_t max_tasks;
    uint32_t reserved;
    uint64_t max_payload_bytes;
} VgTaskGraphBuilderDesc;

typedef struct VgExecutionShape {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t flags;
} VgExecutionShape;

/* ---- v1.3 additions (F2/ADR-046, F3.5/ADR-048) ------------------------- */

/* Sample source + attachment target for one raster pass, mirroring
 * core::RasterFacetPair. Adjacent VgFacetRef parameters are otherwise
 * interchangeable at every draw call site. */
typedef struct VgRasterFacetPair {
    VgFacetRef source;
    VgFacetRef target;
} VgRasterFacetPair;

/* Mirrors core::CanonicalView field-for-field. `allocation`/`allocation_generation`
 * are the raw id/generation pair (not a VgAllocation handle) -- obtain them via
 * the existing getAllocationRef(VgAllocation, uint64_t*, uint32_t*) entry point. */
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

/* Mirrors core::TaskRecord field-for-field, with one disclosed extension:
 * 04-public-c-abi.md Sec.17's illustrative `root` is a bare uint64_t with no
 * room for a generation, but core::TaskRecord always carried
 * root_allocation+root_generation split. ADR-044 adds root_generation here
 * rather than silently truncating the identity the C++ type already
 * carries.
 *
 * Recompile hazard (ADR-048, precedented by root_generation's addition
 * above under ADR-044): unlike every other struct in this file,
 * VgTaskRecord carries no per-element VgStructHeader and is passed to
 * taskGraphAppend as a raw array -- there is no runtime size negotiation
 * for it the way VgApi negotiates `size` once per vgGetApi call. Appending
 * the v1.3 fields below therefore changes sizeof(VgTaskRecord), and this is
 * a disclosed, deliberate exception to "no blind tail-appending": an
 * application binary compiled against an older (smaller) header but run
 * against a newer libvg will have taskGraphAppend's array indexing computed
 * from the *library's* larger compiled-in sizeof, reading past the end of
 * the caller's own array. Callers MUST recompile against the current
 * header when linking against a libvg built from this version.
 *
 * Zero-init defaulting mismatch: a zero-initialized VgTaskRecord correctly
 * decodes kind = VG_TASK_KIND_COMPUTE, topology = VG_TOPOLOGY_TRIANGLE_LIST
 * and raster_wrap = VG_WRAP_CLAMP (all ordinal 0, matching the internal
 * engine's real defaults), but does NOT correctly default raster_filter
 * (zero decodes as VG_FILTER_NEAREST, while core::TaskRecord's actual
 * default is FilterMode::Bilinear) or raster_tint (zero decodes as
 * {0,0,0,0}, while the internal default is {1,1,1,1}, i.e. opaque white).
 * Fields are copied through as-is with no default-substitution -- a caller
 * submitting a VG_TASK_KIND_RASTER task must explicitly set raster_filter
 * and raster_tint rather than relying on zero-init. */
typedef struct VgTaskRecord {
    VgNodeRef node;
    uint64_t root;
    uint32_t root_generation;
    VgExecutionShape shape;
    uint32_t contract_index;
    uint32_t payload_size;
    uint64_t payload_or_offset;
    /* ---- v1.3 additions (F2/ADR-046, F3.5/ADR-048); see the recompile-hazard
     * and zero-init-defaulting-mismatch notes above. ---- */
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

typedef struct VgSealDesc {
    VgStructHeader header;
    uint32_t reserved;
} VgSealDesc;

/* One declared access range, translated at envelope-creation time to a
 * whole-allocation identity (ADR-044 disclosed v1 narrowing: offset/size/
 * access_mask are recorded for a future range-granular certificate but not
 * yet enforced at that granularity -- see core::ExecutionEnvelope). */
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
} VgAccessCertificateDesc;

/* Combines authorization (allowed_nodes), an access certificate, a
 * per-submit task quota and the timeline wait/signal values one submit()
 * call authorizes -- 04-public-c-abi.md Sec.17's "envelope 组合 authorization
 * + certificate + epoch + quota + timeline". v1.1 narrowing (ADR-044): one
 * timeline per device, so `arena` and the timeline values here are the only
 * ones submit() honors; multi-timeline N-wait/N-signal is deferred. */
typedef struct VgExecutionEnvelopeDesc {
    VgStructHeader header;
    VgArena arena;
    const VgNodeRef* allowed_nodes;
    uint32_t allowed_node_count;
    const VgAccessCertificateDesc* access_certificate;
    VgBool32 has_task_quota;
    uint32_t task_quota;
    uint64_t timeline_wait_value;
    uint64_t timeline_signal_value;
} VgExecutionEnvelopeDesc;

typedef struct VgSubmitDesc {
    VgStructHeader header;
    VgTaskGraph graph;
    VgExecutionEnvelope envelope;
    uint64_t flags;
} VgSubmitDesc;

typedef struct VgApi {
    uint32_t version;
    uint32_t size;
    VgResult (VG_CALL *createRuntime)(const VgRuntimeDesc* desc, VgRuntime* out_runtime);
    void (VG_CALL *destroyRuntime)(VgRuntime runtime);
    VgResult (VG_CALL *enumerateAdapters)(VgRuntime runtime, uint32_t* inout_count,
                                           VgAdapterInfo* out_infos);
    /* ---- v1.1 (ADR-043 Decision #2 / ADR-044); populated only when the
     * caller requests VG_API_VERSION_1_1 or later from vgGetApi. Strictly
     * append-only past this point: the five members above are byte-identical
     * to v1.0 in offset and meaning. ---- */
    VgResult (VG_CALL *openAdapter)(VgRuntime runtime, const uint8_t uuid[16], VgAdapter* out_adapter);
    void (VG_CALL *closeAdapter)(VgAdapter adapter);
    VgResult (VG_CALL *createDevice)(VgAdapter adapter, const VgDeviceDesc* desc, VgDevice* out_device);
    void (VG_CALL *destroyDevice)(VgDevice device);
    VgResult (VG_CALL *createAddressDomain)(VgDevice device, const VgAddressDomainDesc* desc,
                                             VgAddressDomain* out_domain);
    void (VG_CALL *destroyAddressDomain)(VgAddressDomain domain);
    VgResult (VG_CALL *createArena)(VgDevice device, const VgArenaDesc* desc, VgArena* out_arena);
    void (VG_CALL *destroyArena)(VgArena arena);
    VgResult (VG_CALL *arenaAllocate)(VgArena arena, uint64_t size, VgAllocation* out_allocation);
    VgResult (VG_CALL *getAllocationRef)(VgAllocation allocation, uint64_t* out_id,
                                          uint32_t* out_generation);
    VgResult (VG_CALL *loadCodeObject)(VgDevice device, const VgCodeObjectDesc* desc,
                                        VgCodeObject* out_code_object);
    void (VG_CALL *destroyCodeObject)(VgCodeObject code_object);
    VgResult (VG_CALL *createNode)(VgCodeObject code_object, const VgNodeDesc* desc, VgNode* out_node);
    void (VG_CALL *destroyNode)(VgNode node);
    VgResult (VG_CALL *getNodeRef)(VgNode node, VgNodeRef* out_ref);
    VgResult (VG_CALL *createTaskGraphBuilder)(VgDevice device, const VgTaskGraphBuilderDesc* desc,
                                                VgTaskGraphBuilder* out_builder);
    void (VG_CALL *destroyTaskGraphBuilder)(VgTaskGraphBuilder builder);
    VgResult (VG_CALL *taskGraphAppend)(VgTaskGraphBuilder builder, const VgTaskRecord* tasks,
                                         uint32_t task_count, VgTaskId* out_ids);
    VgResult (VG_CALL *taskGraphAddDependency)(VgTaskGraphBuilder builder, VgTaskId before, VgTaskId after);
    VgResult (VG_CALL *sealTaskGraph)(VgTaskGraphBuilder builder, const VgSealDesc* desc,
                                       VgTaskGraph* out_graph);
    void (VG_CALL *destroyTaskGraph)(VgTaskGraph graph);
    VgResult (VG_CALL *createExecutionEnvelope)(VgDevice device, const VgExecutionEnvelopeDesc* desc,
                                                 VgExecutionEnvelope* out_envelope);
    void (VG_CALL *destroyExecutionEnvelope)(VgExecutionEnvelope envelope);
    VgResult (VG_CALL *createTimeline)(VgDevice device, VgTimeline* out_timeline);
    void (VG_CALL *destroyTimeline)(VgTimeline timeline);
    VgResult (VG_CALL *waitTimeline)(VgTimeline timeline, uint64_t value);
    VgResult (VG_CALL *submit)(VgDevice device, const VgSubmitDesc* submit_desc, VgSubmission* out_submission);
    void (VG_CALL *destroySubmission)(VgSubmission submission);
    VgResult (VG_CALL *getSubmissionLoweringReport)(VgSubmission submission, const char** out_json);
    /* ---- v1.2 (ADR-045); populated only when the caller requests
     * VG_API_VERSION_1_2 or later from vgGetApi. Strictly append-only past
     * this point, same discipline as the v1.0->v1.1 boundary above: every
     * member before this line keeps its exact v1.1 offset and meaning. ----
     * submit() always discarded hal::Submission::result -- VG_SUCCESS from
     * submit() meant only "the submission mechanism accepted the plan,"
     * never "the execution actually succeeded" (a fault/poison outcome was
     * unreachable through v1.1). getSubmissionExecutionResult returns the
     * canonical JSON serialization of core::ExecutionResult (ok, poison,
     * message, fault, witness entries, missing_effects, outputs_valid),
     * valid until the submission is destroyed -- same shape/lifetime
     * contract as getSubmissionLoweringReport. */
    VgResult (VG_CALL *getSubmissionExecutionResult)(VgSubmission submission, const char** out_json);
    /* ---- v1.3 (F2/ADR-046, F3.5/ADR-048); populated only when the caller
     * requests VG_API_VERSION_1_3 or later from vgGetApi. Strictly
     * append-only past this point, same discipline as the v1.1->v1.2
     * boundary above: every member before this line keeps its exact v1.2
     * offset and meaning. Raster reaches the public C-ABI: acquireFacet is
     * the public entry point onto core::FacetPool::acquire, letting a
     * caller obtain the VgFacetRef a VG_TASK_KIND_RASTER VgTaskRecord's
     * raster_facets/vertex_buffer_ref/index_buffer_ref fields require. */
    VgResult (VG_CALL *acquireFacet)(VgDevice device, VgArena arena, const VgCanonicalViewDesc* view,
                                      uint32_t facet_kind, VgFacetRef* out_facet);
} VgApi;

#define VG_INIT_STRUCT(struct_type, structure_type) \
    { { (structure_type), (uint32_t)sizeof(struct_type), NULL } }

VG_API VgResult VG_CALL vgGetApi(uint32_t requested_version, VgApi* out_api);
VG_API const char* VG_CALL vgGetLastDiagnostic(void);

#ifdef __cplusplus
}
#endif
#endif
