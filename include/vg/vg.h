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
    VG_LOG_INFO = 3u
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

typedef struct VgApi {
    uint32_t version;
    uint32_t size;
    VgResult (VG_CALL *createRuntime)(const VgRuntimeDesc* desc, VgRuntime* out_runtime);
    void (VG_CALL *destroyRuntime)(VgRuntime runtime);
    VgResult (VG_CALL *enumerateAdapters)(VgRuntime runtime, uint32_t* inout_count,
                                           VgAdapterInfo* out_infos);
} VgApi;

#define VG_INIT_STRUCT(struct_type, structure_type) \
    { { (structure_type), (uint32_t)sizeof(struct_type), NULL } }

VG_API VgResult VG_CALL vgGetApi(uint32_t requested_version, VgApi* out_api);
VG_API const char* VG_CALL vgGetLastDiagnostic(void);

#ifdef __cplusplus
}
#endif
#endif
