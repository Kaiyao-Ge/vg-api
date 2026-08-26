#include "vg/vg.h"
#include "api/vg_api_internal.h"
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

namespace {
std::mutex g_runtime_mutex;
std::unordered_set<VgRuntime_T*> g_runtimes;
thread_local std::string g_diagnostic;
}  // namespace

namespace vg_api {

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

}  // namespace vg_api

namespace {

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
    vg_api::set_diagnostic("runtime descriptor and output handle are required");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  const VgResult header_result =
      vg_api::validate_header(desc->header, VG_STRUCTURE_RUNTIME_DESC, sizeof(VgRuntimeDesc));
  if (header_result != VG_SUCCESS) return header_result;
  if ((desc->allocate == nullptr) != (desc->free == nullptr)) {
    vg_api::set_diagnostic("runtime allocator callbacks must be supplied as a pair");
    return VG_ERROR_INVALID_ARGUMENT;
  }

  void* storage = desc->allocate != nullptr
      ? desc->allocate(desc->user, sizeof(VgRuntime_T), alignof(VgRuntime_T))
      : ::operator new(sizeof(VgRuntime_T), std::nothrow);
  if (storage == nullptr) {
    vg_api::set_diagnostic("runtime allocation failed");
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
  if (inout_count == nullptr || !vg_api::has_runtime(runtime)) {
    vg_api::set_diagnostic("runtime is invalid or adapter count is missing");
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
    const VgResult header_result =
        vg_api::validate_header(output.header, VG_STRUCTURE_ADAPTER_INFO, sizeof(VgAdapterInfo));
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
  // v1.0's layout is exactly the first 5 members (version, size, and the
  // three original function pointers); v1.1 adds the golden-path chain
  // through getSubmissionLoweringReport; v1.2 (ADR-045) appends
  // getSubmissionExecutionResult; v1.3 (F2/ADR-046, F3.5/ADR-048) appends
  // acquireFacet; v1.4 appends taskGraphAppendV2. v1.5 adds F5 semantics to
  // the existing V2 record but deliberately does not grow VgApi. Using the version-appropriate boundary here (rather than
  // always requiring sizeof(VgApi)) is what lets an older caller with an
  // older-sized output buffer succeed against this newer library -- the
  // actual backward-compatibility contract offsetof/size negotiation exists
  // for.
  const size_t v1_0_size = offsetof(VgApi, openAdapter);
  const size_t v1_1_size = offsetof(VgApi, getSubmissionExecutionResult);
  const size_t v1_2_size = offsetof(VgApi, acquireFacet);
  const size_t v1_3_size = offsetof(VgApi, taskGraphAppendV2);
  if (out_api == nullptr ||
      (requested_version != VG_API_VERSION_1_0 && requested_version != VG_API_VERSION_1_1 &&
       requested_version != VG_API_VERSION_1_2 && requested_version != VG_API_VERSION_1_3 &&
       requested_version != VG_API_VERSION_1_4 && requested_version != VG_API_VERSION_1_5)) {
    vg_api::set_diagnostic("requested API version is unsupported");
    return VG_ERROR_INVALID_ARGUMENT;
  }
  const bool at_least_v1_1 = requested_version == VG_API_VERSION_1_1 || requested_version == VG_API_VERSION_1_2 ||
                              requested_version == VG_API_VERSION_1_3 || requested_version == VG_API_VERSION_1_4 || requested_version == VG_API_VERSION_1_5;
  const bool at_least_v1_2 = requested_version == VG_API_VERSION_1_2 || requested_version == VG_API_VERSION_1_3 ||
                              requested_version == VG_API_VERSION_1_4 || requested_version == VG_API_VERSION_1_5;
  const bool at_least_v1_3 = requested_version == VG_API_VERSION_1_3 || requested_version == VG_API_VERSION_1_4 || requested_version == VG_API_VERSION_1_5;
  const bool at_least_v1_4 = requested_version == VG_API_VERSION_1_4 || requested_version == VG_API_VERSION_1_5;
  const size_t full_size =
      at_least_v1_4 ? sizeof(VgApi)
                    : (at_least_v1_3 ? v1_3_size : (at_least_v1_2 ? v1_2_size : (at_least_v1_1 ? v1_1_size : v1_0_size)));
  if (out_api->size < full_size) {
    vg_api::set_diagnostic("output API table is too small for the requested version");
    return VG_ERROR_INVALID_ARGUMENT;
  }

  VgApi api{};
  api.version = requested_version;
  api.size = static_cast<uint32_t>(full_size);
  api.createRuntime = create_runtime;
  api.destroyRuntime = destroy_runtime;
  api.enumerateAdapters = enumerate_adapters;
  if (at_least_v1_1) {
    api.openAdapter = vg_api::open_adapter;
    api.closeAdapter = vg_api::close_adapter;
    api.createDevice = vg_api::create_device;
    api.destroyDevice = vg_api::destroy_device;
    api.createAddressDomain = vg_api::create_address_domain;
    api.destroyAddressDomain = vg_api::destroy_address_domain;
    api.createArena = vg_api::create_arena;
    api.destroyArena = vg_api::destroy_arena;
    api.arenaAllocate = vg_api::arena_allocate;
    api.getAllocationRef = vg_api::get_allocation_ref;
    api.loadCodeObject = vg_api::load_code_object;
    api.destroyCodeObject = vg_api::destroy_code_object;
    api.createNode = vg_api::create_node;
    api.destroyNode = vg_api::destroy_node;
    api.getNodeRef = vg_api::get_node_ref;
    api.createTaskGraphBuilder = vg_api::create_task_graph_builder;
    api.destroyTaskGraphBuilder = vg_api::destroy_task_graph_builder;
    api.taskGraphAppend = vg_api::task_graph_append;
    api.taskGraphAddDependency = vg_api::task_graph_add_dependency;
    api.sealTaskGraph = vg_api::seal_task_graph;
    api.destroyTaskGraph = vg_api::destroy_task_graph;
    api.createExecutionEnvelope = vg_api::create_execution_envelope;
    api.destroyExecutionEnvelope = vg_api::destroy_execution_envelope;
    api.createTimeline = vg_api::create_timeline;
    api.destroyTimeline = vg_api::destroy_timeline;
    api.waitTimeline = vg_api::wait_timeline;
    api.submit = vg_api::submit;
    api.destroySubmission = vg_api::destroy_submission;
    api.getSubmissionLoweringReport = vg_api::get_submission_lowering_report;
  }
  if (at_least_v1_2) {
    api.getSubmissionExecutionResult = vg_api::get_submission_execution_result;
  }
  if (at_least_v1_3) {
    api.acquireFacet = vg_api::acquire_facet;
  }
  if (at_least_v1_4) {
    api.taskGraphAppendV2 = vg_api::task_graph_append_v2;
  }
  std::memcpy(out_api, &api, full_size);
  return VG_SUCCESS;
}

extern "C" VG_API const char* VG_CALL vgGetLastDiagnostic(void) { return g_diagnostic.c_str(); }
