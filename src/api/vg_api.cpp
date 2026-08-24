#include "vg/vg.h"
#include "backends/probe.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_set>
#include <vector>

struct VgRuntime_T {
  void* user{};
  VgAllocateFn allocate{};
  VgFreeFn free_fn{};
  VgLogFn log{};
  uint32_t validation_profile{};
  std::vector<vg::AdapterRecord> adapters;
};

namespace {
std::mutex g_runtime_mutex;
std::unordered_set<VgRuntime_T*> g_runtimes;
thread_local std::string g_diagnostic;

void set_diagnostic(const char* message) { g_diagnostic = message; }

bool has_runtime(VgRuntime runtime) {
  std::scoped_lock lock(g_runtime_mutex);
  return g_runtimes.contains(runtime);
}

VgResult validate_extensions(const VgStructHeader& header) {
  std::unordered_set<const void*> visited;
  const auto* extension = static_cast<const VgStructHeader*>(header.next);
  while (extension != nullptr) {
    if (!visited.insert(extension).second || extension->size < sizeof(VgStructHeader)) {
      set_diagnostic("extension chain is cyclic or has an invalid header size");
      return VG_ERROR_INVALID_ARGUMENT;
    }
    if ((extension->type & VG_STRUCTURE_REQUIRED_BIT) != 0) {
      set_diagnostic("required extension is not supported by Phase 0");
      return VG_ERROR_UNSUPPORTED;
    }
    extension = static_cast<const VgStructHeader*>(extension->next);
  }
  return VG_SUCCESS;
}

VgResult validate_header(const VgStructHeader& header, uint32_t type, size_t full_size) {
  if (header.type != type || header.size < full_size) {
    set_diagnostic("structure has an invalid type or size");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  return validate_extensions(header);
}

void VG_CALL destroy_runtime(VgRuntime runtime) {
  if (runtime == nullptr) return;
  {
    std::scoped_lock lock(g_runtime_mutex);
    if (!g_runtimes.erase(runtime)) return;
  }
  VgFreeFn free_fn = runtime->free_fn;
  void* user = runtime->user;
  runtime->~VgRuntime_T();
  if (free_fn != nullptr) {
    free_fn(user, runtime);
  } else {
    ::operator delete(runtime);
  }
}

VgResult VG_CALL create_runtime(const VgRuntimeDesc* desc, VgRuntime* out_runtime) {
  if (desc == nullptr || out_runtime == nullptr) {
    set_diagnostic("runtime descriptor and output handle are required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  const VgResult header_result = validate_header(desc->header, VG_STRUCTURE_RUNTIME_DESC, sizeof(VgRuntimeDesc));
  if (header_result != VG_SUCCESS) return header_result;
  if ((desc->allocate == nullptr) != (desc->free == nullptr)) {
    set_diagnostic("runtime allocator callbacks must be supplied as a pair");
    return VG_ERROR_INVALID_ARGUMENT;
  }

  void* storage = desc->allocate != nullptr
      ? desc->allocate(desc->user, sizeof(VgRuntime_T), alignof(VgRuntime_T))
      : ::operator new(sizeof(VgRuntime_T), std::nothrow);
  if (storage == nullptr) {
    set_diagnostic("runtime allocation failed");
    return VG_ERROR_OUT_OF_HOST_MEMORY;
  }
  auto* runtime = new (storage) VgRuntime_T();
  runtime->user = desc->user;
  runtime->allocate = desc->allocate;
  runtime->free_fn = desc->free;
  runtime->log = desc->log;
  runtime->validation_profile = desc->validation_profile;
  runtime->adapters = vg::reference_adapters();
  const auto metal = vg::metal_adapters();
  const auto vulkan = vg::vulkan_adapters();
  runtime->adapters.insert(runtime->adapters.end(), metal.begin(), metal.end());
  runtime->adapters.insert(runtime->adapters.end(), vulkan.begin(), vulkan.end());
  {
    std::scoped_lock lock(g_runtime_mutex);
    g_runtimes.insert(runtime);
  }
  if (runtime->log != nullptr) runtime->log(runtime->user, VG_LOG_INFO, 0, "VG runtime created");
  *out_runtime = runtime;
  return VG_SUCCESS;
}

VgResult VG_CALL enumerate_adapters(VgRuntime runtime, uint32_t* inout_count,
                                    VgAdapterInfo* out_infos) {
  if (inout_count == nullptr || !has_runtime(runtime)) {
    set_diagnostic("runtime is invalid or adapter count is missing");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  const auto available = static_cast<uint32_t>(runtime->adapters.size());
  if (out_infos == nullptr) {
    *inout_count = available;
    return VG_SUCCESS;
  }
  const uint32_t requested = *inout_count;
  const uint32_t written = std::min(requested, available);
  for (uint32_t i = 0; i < written; ++i) {
    const VgAdapterInfo& output = out_infos[i];
    const VgResult header_result = validate_header(output.header, VG_STRUCTURE_ADAPTER_INFO, sizeof(VgAdapterInfo));
    if (header_result != VG_SUCCESS) return header_result;
  }
  for (uint32_t i = 0; i < written; ++i) {
    VgAdapterInfo& output = out_infos[i];
    const vg::AdapterRecord& input = runtime->adapters[i];
    std::memcpy(output.stable_uuid, input.uuid, sizeof(output.stable_uuid));
    output.backend_kind = input.backend_kind;
    output.adapter_class = input.adapter_class;
    std::snprintf(output.name, sizeof(output.name), "%s", input.name.c_str());
    std::snprintf(output.driver, sizeof(output.driver), "%s", input.driver.c_str());
  }
  *inout_count = written;
  return written == available ? VG_SUCCESS : VG_INCOMPLETE;
}
}  // namespace

extern "C" VG_API VgResult VG_CALL vgGetApi(uint32_t requested_version, VgApi* out_api) {
  if (out_api == nullptr || requested_version != VG_API_VERSION_1_0 ||
      out_api->size < sizeof(VgApi)) {
    set_diagnostic("requested API version or output table size is unsupported");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  VgApi api{};
  api.version = VG_API_VERSION_1_0;
  api.size = sizeof(VgApi);
  api.createRuntime = create_runtime;
  api.destroyRuntime = destroy_runtime;
  api.enumerateAdapters = enumerate_adapters;
  std::memcpy(out_api, &api, sizeof(api));
  return VG_SUCCESS;
}

extern "C" VG_API const char* VG_CALL vgGetLastDiagnostic(void) { return g_diagnostic.c_str(); }
