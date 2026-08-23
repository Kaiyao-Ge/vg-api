#include "vg/vg.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(VgBool32) == 4, "VgBool32 must be 32-bit");
_Static_assert(sizeof(VgGpuAddress) == 8, "VgGpuAddress must be 64-bit");
_Static_assert(offsetof(VgRuntimeDesc, header) == 0, "header must be first");
_Static_assert(offsetof(VgAdapterInfo, header) == 0, "header must be first");
_Static_assert(sizeof(VgFacetRef) == 8, "VgFacetRef is a 64-bit capability token");
_Static_assert(_Alignof(VgFacetRef) == 4, "VgFacetRef must not gain padding");
_Static_assert(offsetof(VgFacetRef, index) == 0, "VgFacetRef.index must be first");
_Static_assert(offsetof(VgFacetRef, generation) == 4, "VgFacetRef.generation follows index");
_Static_assert(offsetof(VgRegionDesc, header) == 0, "header must be first");
_Static_assert(offsetof(VgRegionDesc, byte_offset) == offsetof(VgRegionDesc, allocation) + sizeof(VgAllocation),
               "VgRegionDesc byte_offset follows allocation with no padding");
_Static_assert(sizeof(((VgRegionDesc*)0)->shape) == 32, "VgRegionDesc.shape is 4 x uint64");
_Static_assert(sizeof(((VgRegionDesc*)0)->strides) == 32, "VgRegionDesc.strides is 4 x uint64");
_Static_assert(offsetof(VgRegionDesc, rank) == offsetof(VgRegionDesc, layout_class) + 4,
               "VgRegionDesc.rank follows layout_class");
_Static_assert(sizeof(VgRegionDesc) % 8 == 0, "VgRegionDesc must stay 8-byte aligned for capture ABI");

static unsigned g_logs;
static void VG_CALL log_callback(void* user, uint32_t severity, uint32_t category, const char* message) {
  (void)user; (void)severity; (void)category; (void)message; ++g_logs;
}

int main(void) {
  if (vgGetApi(0, NULL) != VG_ERROR_INVALID_ARGUMENT) return 1;
  VgApi too_small = {0};
  too_small.size = (uint32_t)(sizeof(VgApi) - 1);
  if (vgGetApi(VG_API_VERSION_1_0, &too_small) != VG_ERROR_INVALID_ARGUMENT) return 2;
  VgApi api = {0};
  api.size = sizeof(api);
  if (vgGetApi(VG_API_VERSION_1_0, &api) != VG_SUCCESS || api.createRuntime == NULL) return 3;

  VgRuntimeDesc bad = VG_INIT_STRUCT(VgRuntimeDesc, VG_STRUCTURE_RUNTIME_DESC);
  bad.allocate = (VgAllocateFn)log_callback;
  VgRuntime untouched = (VgRuntime)(uintptr_t)0x1;
  if (api.createRuntime(&bad, &untouched) != VG_ERROR_INVALID_ARGUMENT || untouched != (VgRuntime)(uintptr_t)0x1) return 4;

  VgRuntimeDesc desc = VG_INIT_STRUCT(VgRuntimeDesc, VG_STRUCTURE_RUNTIME_DESC);
  desc.log = log_callback;
  VgStructHeader optional_extension = { 99u, sizeof(VgStructHeader), NULL };
  desc.header.next = &optional_extension;
  VgRuntime runtime = NULL;
  if (api.createRuntime(&desc, &runtime) != VG_SUCCESS || runtime == NULL || g_logs != 1) return 5;
  uint32_t count = 0;
  if (api.enumerateAdapters(runtime, &count, NULL) != VG_SUCCESS || count == 0) return 6;
  VgAdapterInfo one = VG_INIT_STRUCT(VgAdapterInfo, VG_STRUCTURE_ADAPTER_INFO);
  uint32_t capacity = 1;
  const VgResult status = api.enumerateAdapters(runtime, &capacity, &one);
  if ((status != VG_SUCCESS && status != VG_INCOMPLETE) || capacity != 1 || one.backend_kind != VG_BACKEND_REFERENCE || one.stable_uuid[0] == 0) return 7;
  api.destroyRuntime(runtime);
  api.destroyRuntime(runtime);
  if (api.enumerateAdapters((VgRuntime)(uintptr_t)0x2, &capacity, &one) != VG_ERROR_INVALID_ARGUMENT) return 8;
  VgRuntimeDesc required = VG_INIT_STRUCT(VgRuntimeDesc, VG_STRUCTURE_RUNTIME_DESC);
  VgStructHeader required_extension = { VG_STRUCTURE_REQUIRED_BIT | 1u, sizeof(VgStructHeader), NULL };
  required.header.next = &required_extension;
  if (api.createRuntime(&required, &runtime) != VG_ERROR_UNSUPPORTED) return 9;
  return 0;
}
