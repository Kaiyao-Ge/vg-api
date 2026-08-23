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
    VG_FACET_KIND_ADDRESS = 0u,
    VG_FACET_KIND_SAMPLE = 1u,
    VG_FACET_KIND_STORAGE = 2u,
    VG_FACET_KIND_ATTACHMENT = 3u,
    VG_FACET_KIND_TRANSFER = 4u,
    VG_VALIDATION_DISABLED = 0u,
    VG_VALIDATION_REFERENCE_STRICT = 1u,
    VG_VALIDATION_CHECKED_NATIVE = 2u,
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
