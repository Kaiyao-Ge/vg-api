// F1 (ADR-043 Decision #2 / ADR-044): the actual Checkpoint-A precondition.
// Includes only <vg/vg.h> and links only vg_api -- proves the full v1.1
// golden path, stale-handle rejection, and version skew are all reachable
// through the public C ABI alone, never touching vg_core/vg_backend_reference
// directly.
#include "vg/vg.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <cstdio>
#include <string>
#include <vector>

namespace {

bool g_ok = true;

bool check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s (diagnostic: %s)\n", what, vgGetLastDiagnostic());
    g_ok = false;
  }
  return condition;
}

}  // namespace

int main() {
  VgApi api{};
  api.size = sizeof(api);
  if (!check(vgGetApi(VG_API_VERSION_1_1, &api) == VG_SUCCESS, "vgGetApi v1.1")) return 1;
  check(api.version == VG_API_VERSION_1_1, "api.version == v1.1");
  // v1.2 (ADR-045) appended getSubmissionExecutionResult after this test's
  // v1.1 request boundary, so sizeof(VgApi) now overshoots what a v1.1
  // caller gets back -- the v1.1/v1.2 boundary is offsetof(VgApi,
  // getSubmissionExecutionResult), mirroring the v1.0/v1.1 boundary check
  // further down this file.
  check(api.size == offsetof(VgApi, getSubmissionExecutionResult),
        "api.size == v1.1/v1.2 boundary");

  VgRuntimeDesc runtime_desc{};
  runtime_desc.header.type = VG_STRUCTURE_RUNTIME_DESC;
  runtime_desc.header.size = sizeof(runtime_desc);
  VgRuntime runtime = nullptr;
  if (!check(api.createRuntime(&runtime_desc, &runtime) == VG_SUCCESS, "createRuntime")) return 1;

  uint32_t adapter_count = 0;
  check(api.enumerateAdapters(runtime, &adapter_count, nullptr) == VG_SUCCESS, "enumerateAdapters count");
  check(adapter_count > 0, "at least one adapter enumerated");

  std::vector<VgAdapterInfo> adapters(adapter_count);
  for (auto& info : adapters) {
    info.header.type = VG_STRUCTURE_ADAPTER_INFO;
    info.header.size = sizeof(VgAdapterInfo);
  }
  uint32_t written = adapter_count;
  check(api.enumerateAdapters(runtime, &written, adapters.data()) == VG_SUCCESS, "enumerateAdapters fill");

  // Prefer a Metal-class adapter when one is enumerated (real hardware
  // coverage of the ADR-044 uuid-selective openAdapter/createDevice path);
  // fall back to the reference backend, which is always available, so the
  // golden path stays host-independent on machines without a GPU.
  const VgAdapterInfo* chosen = nullptr;
  for (const auto& info : adapters) {
    if (info.backend_kind == VG_BACKEND_METAL) {
      chosen = &info;
      break;
    }
  }
  if (chosen == nullptr) {
    for (const auto& info : adapters) {
      if (info.backend_kind == VG_BACKEND_REFERENCE) {
        chosen = &info;
        break;
      }
    }
  }
  if (!check(chosen != nullptr, "a usable adapter (Metal or reference) is present")) return 1;

  VgAdapter adapter = nullptr;
  check(api.openAdapter(runtime, chosen->stable_uuid, &adapter) == VG_SUCCESS, "openAdapter");

  VgDeviceDesc device_desc{};
  device_desc.header.type = VG_STRUCTURE_DEVICE_DESC;
  device_desc.header.size = sizeof(device_desc);
  VgDevice device = nullptr;
  check(api.createDevice(adapter, &device_desc, &device) == VG_SUCCESS, "createDevice");

  VgAddressDomainDesc domain_desc{};
  domain_desc.header.type = VG_STRUCTURE_ADDRESS_DOMAIN_DESC;
  domain_desc.header.size = sizeof(domain_desc);
  domain_desc.kind = VG_ADDRESS_DOMAIN_DEVICE_LOCAL;
  VgAddressDomain domain = nullptr;
  check(api.createAddressDomain(device, &domain_desc, &domain) == VG_SUCCESS, "createAddressDomain");

  VgArenaDesc arena_desc{};
  arena_desc.header.type = VG_STRUCTURE_ARENA_DESC;
  arena_desc.header.size = sizeof(arena_desc);
  arena_desc.domain = domain;
  VgArena arena = nullptr;
  check(api.createArena(device, &arena_desc, &arena) == VG_SUCCESS, "createArena");

  VgAllocation allocation = nullptr;
  check(api.arenaAllocate(arena, 64, &allocation) == VG_SUCCESS, "arenaAllocate");
  // F7 is deliberately a copy API over the same Arena allocation model, not
  // a second upload/readback object family. Exercise its owner/range checks
  // before the allocation participates in the existing public graph.
  VgApi io_api{};
  io_api.size = sizeof(io_api);
  check(vgGetApi(VG_API_VERSION_1_6, &io_api) == VG_SUCCESS, "vgGetApi v1.6");
  check(io_api.writeAllocation != nullptr && io_api.readAllocation != nullptr,
        "v1.6 allocation I/O functions are populated");
  const uint8_t host_bytes[] = {7, 9, 11, 13};
  uint8_t copied_bytes[4]{};
  check(io_api.writeAllocation(arena, allocation, 8, host_bytes, sizeof(host_bytes)) == VG_SUCCESS,
        "writeAllocation round trip write");
  check(io_api.readAllocation(arena, allocation, 8, copied_bytes, sizeof(copied_bytes)) == VG_SUCCESS &&
            std::memcmp(host_bytes, copied_bytes, sizeof(host_bytes)) == 0,
        "readAllocation round trip read");
  check(io_api.writeAllocation(arena, allocation, 63, host_bytes, sizeof(host_bytes)) == VG_ERROR_INVALID_ARGUMENT,
        "writeAllocation rejects out-of-range write");
  check(io_api.readAllocation(arena, allocation, 0, nullptr, 1) == VG_ERROR_INVALID_ARGUMENT,
        "readAllocation rejects null destination");
  check(io_api.writeAllocation(arena, allocation, std::numeric_limits<uint64_t>::max(), host_bytes, 1) ==
            VG_ERROR_INVALID_ARGUMENT,
        "writeAllocation rejects uint64 overflow offset");
  VgAllocation impossible_allocation = nullptr;
  check(io_api.arenaAllocate(arena, std::numeric_limits<uint64_t>::max(), &impossible_allocation) != VG_SUCCESS,
        "arenaAllocate rejects unrepresentable size");
  uint64_t allocation_id = 0;
  uint32_t allocation_generation = 0;
  check(api.getAllocationRef(allocation, &allocation_id, &allocation_generation) == VG_SUCCESS, "getAllocationRef");

  const std::string module_json =
      "{\"schema\":\"vg.ir/v1\",\"version\":1,\"root_schema\":\"vg.test/v1\",\"instructions\":[{\"op\":\"load\","
      "\"allocation\":" +
      std::to_string(allocation_id) + ",\"generation\":" + std::to_string(allocation_generation) +
      ",\"offset\":0,\"size\":4}],\"effects\":[{\"allocation\":" + std::to_string(allocation_id) +
      ",\"offset\":0,\"size\":64,\"access\":\"read\",\"representation_epoch\":0}]}";

  VgCodeObjectDesc code_desc{};
  code_desc.header.type = VG_STRUCTURE_CODE_OBJECT_DESC;
  code_desc.header.size = sizeof(code_desc);
  code_desc.bytes = module_json.data();
  code_desc.byte_size = module_json.size();
  code_desc.format_tag = "vg.ir/v1";
  VgCodeObject code_object = nullptr;
  check(api.loadCodeObject(device, &code_desc, &code_object) == VG_SUCCESS, "loadCodeObject");

  VgNodeDesc node_desc{};
  node_desc.header.type = VG_STRUCTURE_NODE_DESC;
  node_desc.header.size = sizeof(node_desc);
  node_desc.entry_name = "main";
  VgNode node = nullptr;
  check(api.createNode(code_object, &node_desc, &node) == VG_SUCCESS, "createNode");
  VgNodeRef node_ref{};
  check(api.getNodeRef(node, &node_ref) == VG_SUCCESS, "getNodeRef");

  VgTaskGraphBuilderDesc builder_desc{};
  builder_desc.header.type = VG_STRUCTURE_TASK_GRAPH_BUILDER_DESC;
  builder_desc.header.size = sizeof(builder_desc);
  builder_desc.code_object = code_object;
  VgTaskGraphBuilder builder = nullptr;
  check(api.createTaskGraphBuilder(device, &builder_desc, &builder) == VG_SUCCESS, "createTaskGraphBuilder");

  VgTaskRecord tasks[2]{};
  tasks[0].node = node_ref;
  tasks[0].root = allocation_id;
  tasks[0].root_generation = allocation_generation;
  tasks[0].shape = {1, 1, 1, 0};
  tasks[1].node = node_ref;
  tasks[1].root = allocation_id;
  tasks[1].root_generation = allocation_generation;
  tasks[1].shape = {1, 1, 1, 0};

  VgTaskId ids[2]{};
  check(api.taskGraphAppend(builder, tasks, 2, ids) == VG_SUCCESS, "taskGraphAppend");
  check(api.taskGraphAddDependency(builder, ids[0], ids[1]) == VG_SUCCESS, "taskGraphAddDependency");

  VgSealDesc seal_desc{};
  seal_desc.header.type = VG_STRUCTURE_SEAL_DESC;
  seal_desc.header.size = sizeof(seal_desc);
  VgTaskGraph graph = nullptr;
  check(api.sealTaskGraph(builder, &seal_desc, &graph) == VG_SUCCESS, "sealTaskGraph");

  VgExecutionEnvelopeDesc envelope_desc{};
  envelope_desc.header.type = VG_STRUCTURE_EXECUTION_ENVELOPE_DESC;
  envelope_desc.header.size = sizeof(envelope_desc);
  envelope_desc.arena = arena;
  VgExecutionEnvelope envelope = nullptr;
  check(api.createExecutionEnvelope(device, &envelope_desc, &envelope) == VG_SUCCESS, "createExecutionEnvelope");

  VgTimeline timeline = nullptr;
  check(api.createTimeline(device, &timeline) == VG_SUCCESS, "createTimeline");

  VgSubmitDesc submit_desc{};
  submit_desc.header.type = VG_STRUCTURE_SUBMIT_DESC;
  submit_desc.header.size = sizeof(submit_desc);
  submit_desc.graph = graph;
  submit_desc.envelope = envelope;
  VgSubmission submission = nullptr;
  check(api.submit(device, &submit_desc, &submission) == VG_SUCCESS, "submit");

  const char* report_json = nullptr;
  check(api.getSubmissionLoweringReport(submission, &report_json) == VG_SUCCESS, "getSubmissionLoweringReport");
  check(report_json != nullptr && report_json[0] != '\0', "lowering report json non-empty");

  api.destroySubmission(submission);
  api.destroyTimeline(timeline);
  api.destroyExecutionEnvelope(envelope);
  api.destroyTaskGraph(graph);
  api.destroyTaskGraphBuilder(builder);
  api.destroyNode(node);
  api.destroyCodeObject(code_object);

  // Stale-handle rejection (Sec.16): a destroyed arena must reject further
  // use deterministically, not silently succeed or crash.
  VgArenaDesc stale_arena_desc = arena_desc;
  VgArena stale_arena = nullptr;
  check(api.createArena(device, &stale_arena_desc, &stale_arena) == VG_SUCCESS, "createArena (stale-handle setup)");
  api.destroyArena(stale_arena);
  VgAllocation stale_allocation = nullptr;
  check(api.arenaAllocate(stale_arena, 64, &stale_allocation) == VG_ERROR_STALE_HANDLE,
        "arenaAllocate on destroyed arena is rejected");

  VgCodeObjectDesc stale_code_desc = code_desc;
  VgCodeObject stale_code = nullptr;
  check(api.loadCodeObject(device, &stale_code_desc, &stale_code) == VG_SUCCESS, "loadCodeObject (stale-handle setup)");
  api.destroyCodeObject(stale_code);
  VgNodeDesc stale_node_desc = node_desc;
  VgNode stale_node = nullptr;
  check(api.createNode(stale_code, &stale_node_desc, &stale_node) == VG_ERROR_STALE_HANDLE,
        "createNode on destroyed code object is rejected");

  // ADR-044 regression: taskGraphAppend must validate the caller-supplied
  // VgNodeRef against the code object's NodeTable (capability-token
  // semantics), rejecting a destroyed/stale node ref rather than silently
  // baking it into the task graph.
  VgCodeObjectDesc node_ref_code_desc = code_desc;
  VgCodeObject node_ref_code_object = nullptr;
  check(api.loadCodeObject(device, &node_ref_code_desc, &node_ref_code_object) == VG_SUCCESS,
        "loadCodeObject (stale-node-ref setup)");
  VgNodeDesc node_ref_node_desc = node_desc;
  VgNode node_to_destroy = nullptr;
  check(api.createNode(node_ref_code_object, &node_ref_node_desc, &node_to_destroy) == VG_SUCCESS,
        "createNode (stale-node-ref setup)");
  VgNodeRef stale_node_ref{};
  check(api.getNodeRef(node_to_destroy, &stale_node_ref) == VG_SUCCESS, "getNodeRef (stale-node-ref setup)");
  api.destroyNode(node_to_destroy);

  VgTaskGraphBuilderDesc node_ref_builder_desc{};
  node_ref_builder_desc.header.type = VG_STRUCTURE_TASK_GRAPH_BUILDER_DESC;
  node_ref_builder_desc.header.size = sizeof(node_ref_builder_desc);
  node_ref_builder_desc.code_object = node_ref_code_object;
  VgTaskGraphBuilder node_ref_builder = nullptr;
  check(api.createTaskGraphBuilder(device, &node_ref_builder_desc, &node_ref_builder) == VG_SUCCESS,
        "createTaskGraphBuilder (stale-node-ref setup)");

  VgTaskRecord stale_node_task{};
  stale_node_task.node = stale_node_ref;
  stale_node_task.root = allocation_id;
  stale_node_task.root_generation = allocation_generation;
  stale_node_task.shape = {1, 1, 1, 0};
  VgTaskId stale_node_task_id{};
  check(api.taskGraphAppend(node_ref_builder, &stale_node_task, 1, &stale_node_task_id) ==
            VG_ERROR_INVALID_ARGUMENT,
        "taskGraphAppend with destroyed node ref is rejected");

  // A fabricated node ref (never issued by createNode/getNodeRef) must also
  // be rejected, not just a genuinely-destroyed one.
  VgTaskRecord garbage_node_task{};
  garbage_node_task.node = VgNodeRef{9999, 9999};
  garbage_node_task.root = allocation_id;
  garbage_node_task.root_generation = allocation_generation;
  garbage_node_task.shape = {1, 1, 1, 0};
  VgTaskId garbage_node_task_id{};
  check(api.taskGraphAppend(node_ref_builder, &garbage_node_task, 1, &garbage_node_task_id) ==
            VG_ERROR_INVALID_ARGUMENT,
        "taskGraphAppend with fabricated node ref is rejected");

  api.destroyTaskGraphBuilder(node_ref_builder);
  api.destroyCodeObject(node_ref_code_object);

  // Fix #1 regression: submit() must reject a task graph whose code object
  // was destroyed after sealing but before submit() -- previously this
  // dereferenced freed VgCodeObject_T memory (code_object->code.bytes)
  // instead of checking is_valid_code_object() first.
  {
    VgArenaDesc uaf1_arena_desc = arena_desc;
    VgArena uaf1_arena = nullptr;
    check(api.createArena(device, &uaf1_arena_desc, &uaf1_arena) == VG_SUCCESS, "createArena (UAF#1 setup)");
    VgAllocation uaf1_allocation = nullptr;
    check(api.arenaAllocate(uaf1_arena, 64, &uaf1_allocation) == VG_SUCCESS, "arenaAllocate (UAF#1 setup)");
    uint64_t uaf1_alloc_id = 0;
    uint32_t uaf1_alloc_gen = 0;
    check(api.getAllocationRef(uaf1_allocation, &uaf1_alloc_id, &uaf1_alloc_gen) == VG_SUCCESS,
          "getAllocationRef (UAF#1 setup)");

    const std::string uaf1_module_json =
        "{\"schema\":\"vg.ir/v1\",\"version\":1,\"root_schema\":\"vg.test/v1\",\"instructions\":[{\"op\":\"load\","
        "\"allocation\":" +
        std::to_string(uaf1_alloc_id) + ",\"generation\":" + std::to_string(uaf1_alloc_gen) +
        ",\"offset\":0,\"size\":4}],\"effects\":[{\"allocation\":" + std::to_string(uaf1_alloc_id) +
        ",\"offset\":0,\"size\":64,\"access\":\"read\",\"representation_epoch\":0}]}";

    VgCodeObjectDesc uaf1_code_desc{};
    uaf1_code_desc.header.type = VG_STRUCTURE_CODE_OBJECT_DESC;
    uaf1_code_desc.header.size = sizeof(uaf1_code_desc);
    uaf1_code_desc.bytes = uaf1_module_json.data();
    uaf1_code_desc.byte_size = uaf1_module_json.size();
    uaf1_code_desc.format_tag = "vg.ir/v1";
    VgCodeObject uaf1_code = nullptr;
    check(api.loadCodeObject(device, &uaf1_code_desc, &uaf1_code) == VG_SUCCESS, "loadCodeObject (UAF#1 setup)");

    VgNodeDesc uaf1_node_desc{};
    uaf1_node_desc.header.type = VG_STRUCTURE_NODE_DESC;
    uaf1_node_desc.header.size = sizeof(uaf1_node_desc);
    uaf1_node_desc.entry_name = "main";
    VgNode uaf1_node = nullptr;
    check(api.createNode(uaf1_code, &uaf1_node_desc, &uaf1_node) == VG_SUCCESS, "createNode (UAF#1 setup)");
    VgNodeRef uaf1_node_ref{};
    check(api.getNodeRef(uaf1_node, &uaf1_node_ref) == VG_SUCCESS, "getNodeRef (UAF#1 setup)");

    VgTaskGraphBuilderDesc uaf1_builder_desc{};
    uaf1_builder_desc.header.type = VG_STRUCTURE_TASK_GRAPH_BUILDER_DESC;
    uaf1_builder_desc.header.size = sizeof(uaf1_builder_desc);
    uaf1_builder_desc.code_object = uaf1_code;
    VgTaskGraphBuilder uaf1_builder = nullptr;
    check(api.createTaskGraphBuilder(device, &uaf1_builder_desc, &uaf1_builder) == VG_SUCCESS,
          "createTaskGraphBuilder (UAF#1 setup)");

    VgTaskRecord uaf1_task{};
    uaf1_task.node = uaf1_node_ref;
    uaf1_task.root = uaf1_alloc_id;
    uaf1_task.root_generation = uaf1_alloc_gen;
    uaf1_task.shape = {1, 1, 1, 0};
    VgTaskId uaf1_id{};
    check(api.taskGraphAppend(uaf1_builder, &uaf1_task, 1, &uaf1_id) == VG_SUCCESS, "taskGraphAppend (UAF#1 setup)");

    VgSealDesc uaf1_seal_desc{};
    uaf1_seal_desc.header.type = VG_STRUCTURE_SEAL_DESC;
    uaf1_seal_desc.header.size = sizeof(uaf1_seal_desc);
    VgTaskGraph uaf1_graph = nullptr;
    check(api.sealTaskGraph(uaf1_builder, &uaf1_seal_desc, &uaf1_graph) == VG_SUCCESS, "sealTaskGraph (UAF#1 setup)");

    VgExecutionEnvelopeDesc uaf1_envelope_desc{};
    uaf1_envelope_desc.header.type = VG_STRUCTURE_EXECUTION_ENVELOPE_DESC;
    uaf1_envelope_desc.header.size = sizeof(uaf1_envelope_desc);
    uaf1_envelope_desc.arena = uaf1_arena;
    VgExecutionEnvelope uaf1_envelope = nullptr;
    check(api.createExecutionEnvelope(device, &uaf1_envelope_desc, &uaf1_envelope) == VG_SUCCESS,
          "createExecutionEnvelope (UAF#1 setup)");

    // Destroy the code object AFTER the graph is sealed but BEFORE submit --
    // this is the exact UAF window Fix #1 closes.
    api.destroyCodeObject(uaf1_code);

    VgSubmitDesc uaf1_submit_desc{};
    uaf1_submit_desc.header.type = VG_STRUCTURE_SUBMIT_DESC;
    uaf1_submit_desc.header.size = sizeof(uaf1_submit_desc);
    uaf1_submit_desc.graph = uaf1_graph;
    uaf1_submit_desc.envelope = uaf1_envelope;
    VgSubmission uaf1_submission = nullptr;
    check(api.submit(device, &uaf1_submit_desc, &uaf1_submission) == VG_ERROR_STALE_HANDLE,
          "submit() rejects a task graph whose code object was destroyed (Fix #1 regression)");

    // Fix #2 regression, exercised for free here: uaf1_node->code_object is
    // already-destroyed at this point, so destroyNode() below must not
    // dereference it -- it must still erase the node handle without UB.
    api.destroyExecutionEnvelope(uaf1_envelope);
    api.destroyTaskGraph(uaf1_graph);
    api.destroyTaskGraphBuilder(uaf1_builder);
    api.destroyNode(uaf1_node);
    api.destroyArena(uaf1_arena);
  }

  // Fix #3 regression: createExecutionEnvelope must reject an access
  // certificate range whose VgAllocation pointer does not resolve to a live
  // entry in the envelope's own (already-validated) arena, instead of
  // dereferencing an unproven raw pointer that may point at freed
  // core::Arena::allocations_ storage from an already-destroyed arena.
  {
    VgArenaDesc uaf3_arena_a_desc = arena_desc;
    VgArena uaf3_arena_a = nullptr;
    check(api.createArena(device, &uaf3_arena_a_desc, &uaf3_arena_a) == VG_SUCCESS, "createArena A (UAF#3 setup)");
    VgAllocation uaf3_allocation = nullptr;
    check(api.arenaAllocate(uaf3_arena_a, 64, &uaf3_allocation) == VG_SUCCESS, "arenaAllocate A (UAF#3 setup)");
    // Destroy the owning arena -- uaf3_allocation now points at freed memory.
    api.destroyArena(uaf3_arena_a);

    VgArenaDesc uaf3_arena_b_desc = arena_desc;
    VgArena uaf3_arena_b = nullptr;
    check(api.createArena(device, &uaf3_arena_b_desc, &uaf3_arena_b) == VG_SUCCESS, "createArena B (UAF#3 setup)");

    VgAccessRange uaf3_range{};
    uaf3_range.allocation = uaf3_allocation;
    VgAccessCertificateDesc uaf3_cert_desc{};
    uaf3_cert_desc.mode = VG_ACCESS_CERTIFICATE_MODE_CERTIFIED_PINNED;
    uaf3_cert_desc.range_count = 1;
    uaf3_cert_desc.ranges = &uaf3_range;

    VgExecutionEnvelopeDesc uaf3_envelope_desc{};
    uaf3_envelope_desc.header.type = VG_STRUCTURE_EXECUTION_ENVELOPE_DESC;
    uaf3_envelope_desc.header.size = sizeof(uaf3_envelope_desc);
    uaf3_envelope_desc.arena = uaf3_arena_b;
    uaf3_envelope_desc.access_certificate = &uaf3_cert_desc;
    VgExecutionEnvelope uaf3_envelope = nullptr;
    check(api.createExecutionEnvelope(device, &uaf3_envelope_desc, &uaf3_envelope) == VG_ERROR_STALE_HANDLE,
          "createExecutionEnvelope rejects a stale access-certificate allocation pointer (Fix #3 regression)");

    api.destroyArena(uaf3_arena_b);
  }

  // Fix #4 regression (sequential UAF, no concurrency required): taskGraphAppend
  // and sealTaskGraph must reject a builder whose code object was destroyed
  // after createTaskGraphBuilder() succeeded but before either function runs
  // -- previously these dereferenced freed VgCodeObject_T memory
  // (builder->code_object->nodes.lookup(...) in taskGraphAppend, and an
  // unchecked copy of the dangling pointer into the new VgTaskGraph_T in
  // sealTaskGraph) instead of checking is_valid_code_object() first.
  {
    VgCodeObjectDesc uaf4_code_desc{};
    uaf4_code_desc.header.type = VG_STRUCTURE_CODE_OBJECT_DESC;
    uaf4_code_desc.header.size = sizeof(uaf4_code_desc);
    uaf4_code_desc.bytes = module_json.data();
    uaf4_code_desc.byte_size = module_json.size();
    uaf4_code_desc.format_tag = "vg.ir/v1";
    VgCodeObject uaf4_code = nullptr;
    check(api.loadCodeObject(device, &uaf4_code_desc, &uaf4_code) == VG_SUCCESS, "loadCodeObject (UAF#4 setup)");

    VgNodeDesc uaf4_node_desc{};
    uaf4_node_desc.header.type = VG_STRUCTURE_NODE_DESC;
    uaf4_node_desc.header.size = sizeof(uaf4_node_desc);
    uaf4_node_desc.entry_name = "main";
    VgNode uaf4_node = nullptr;
    check(api.createNode(uaf4_code, &uaf4_node_desc, &uaf4_node) == VG_SUCCESS, "createNode (UAF#4 setup)");
    VgNodeRef uaf4_node_ref{};
    check(api.getNodeRef(uaf4_node, &uaf4_node_ref) == VG_SUCCESS, "getNodeRef (UAF#4 setup)");

    VgTaskGraphBuilderDesc uaf4_builder_desc{};
    uaf4_builder_desc.header.type = VG_STRUCTURE_TASK_GRAPH_BUILDER_DESC;
    uaf4_builder_desc.header.size = sizeof(uaf4_builder_desc);
    uaf4_builder_desc.code_object = uaf4_code;
    VgTaskGraphBuilder uaf4_builder = nullptr;
    check(api.createTaskGraphBuilder(device, &uaf4_builder_desc, &uaf4_builder) == VG_SUCCESS,
          "createTaskGraphBuilder (UAF#4 setup)");

    // Destroy the code object AFTER the builder is created but BEFORE
    // taskGraphAppend/sealTaskGraph -- this is the exact UAF window Fix #4
    // closes.
    api.destroyCodeObject(uaf4_code);

    VgTaskRecord uaf4_task{};
    uaf4_task.node = uaf4_node_ref;
    uaf4_task.root = 0;
    uaf4_task.root_generation = 0;
    uaf4_task.shape = {1, 1, 1, 0};
    VgTaskId uaf4_id{};
    check(api.taskGraphAppend(uaf4_builder, &uaf4_task, 1, &uaf4_id) == VG_ERROR_STALE_HANDLE,
          "taskGraphAppend rejects a builder whose code object was destroyed (Fix #4 regression)");

    VgSealDesc uaf4_seal_desc{};
    uaf4_seal_desc.header.type = VG_STRUCTURE_SEAL_DESC;
    uaf4_seal_desc.header.size = sizeof(uaf4_seal_desc);
    VgTaskGraph uaf4_graph = nullptr;
    check(api.sealTaskGraph(uaf4_builder, &uaf4_seal_desc, &uaf4_graph) == VG_ERROR_STALE_HANDLE,
          "sealTaskGraph rejects a builder whose code object was destroyed (Fix #4 regression)");

    // uaf4_node->code_object is already-destroyed here too -- destroyNode
    // (Fix #2 precedent) must not dereference it, and destroyTaskGraphBuilder
    // must still cleanly erase the handle despite the rejected calls above.
    api.destroyTaskGraphBuilder(uaf4_builder);
    api.destroyNode(uaf4_node);
  }

  api.destroyArena(arena);
  api.destroyAddressDomain(domain);
  api.destroyDevice(device);
  api.closeAdapter(adapter);
  api.destroyRuntime(runtime);

  // Version skew (Sec.16), direction (a): request v1.0 from the v1.1
  // runtime -- the returned table must be exactly the v1.0-shaped subset.
  {
    VgApi v10_api{};
    v10_api.size = sizeof(v10_api);
    check(vgGetApi(VG_API_VERSION_1_0, &v10_api) == VG_SUCCESS, "vgGetApi v1.0");
    check(v10_api.version == VG_API_VERSION_1_0, "v1.0 api.version");
    check(v10_api.size == offsetof(VgApi, openAdapter), "v1.0 api.size matches the v1.0/v1.1 boundary");

    VgRuntimeDesc desc{};
    desc.header.type = VG_STRUCTURE_RUNTIME_DESC;
    desc.header.size = sizeof(desc);
    VgRuntime rt = nullptr;
    check(v10_api.createRuntime(&desc, &rt) == VG_SUCCESS, "v1.0 createRuntime");
    uint32_t count = 0;
    check(v10_api.enumerateAdapters(rt, &count, nullptr) == VG_SUCCESS, "v1.0 enumerateAdapters");
    v10_api.destroyRuntime(rt);
  }

  // Version skew (Sec.16), direction (b): a caller built against the
  // pre-F1, v1.0-only VgApi layout (captured here as a local struct mirroring
  // that shape, per the plan -- not a second copy of the real header) must
  // still work unmodified against this v1.1 library, since v1.1 only grew
  // VgApi by strict append.
  {
    struct LegacyVgApi {
      uint32_t version;
      uint32_t size;
      VgResult(VG_CALL* createRuntime)(const VgRuntimeDesc* desc, VgRuntime* out_runtime);
      void(VG_CALL* destroyRuntime)(VgRuntime runtime);
      VgResult(VG_CALL* enumerateAdapters)(VgRuntime runtime, uint32_t* inout_count, VgAdapterInfo* out_infos);
    };
    LegacyVgApi legacy{};
    legacy.size = sizeof(legacy);
    check(vgGetApi(VG_API_VERSION_1_0, reinterpret_cast<VgApi*>(&legacy)) == VG_SUCCESS,
          "legacy-layout vgGetApi v1.0");

    VgRuntimeDesc desc{};
    desc.header.type = VG_STRUCTURE_RUNTIME_DESC;
    desc.header.size = sizeof(desc);
    VgRuntime rt = nullptr;
    check(legacy.createRuntime(&desc, &rt) == VG_SUCCESS, "legacy createRuntime");
    uint32_t count = 0;
    check(legacy.enumerateAdapters(rt, &count, nullptr) == VG_SUCCESS, "legacy enumerateAdapters");
    legacy.destroyRuntime(rt);
  }

  // ------------------------------------------------------------------------
  // F3.5 (ADR-046/ADR-048): a genuine end-to-end raster submission through
  // nothing but the public C-ABI -- vgGetApi(v1.3) -> createRuntime ->
  // openAdapter/createDevice -> createArena -> loadCodeObject
  // ("vg.msl.raster/v1") -> acquireFacet (source/target/vertex) ->
  // createTaskGraphBuilder/createNode -> taskGraphAppend(VG_TASK_KIND_RASTER)
  // -> sealTaskGraph -> submit -> getSubmissionExecutionResult.
  //
  // Two structural gaps in the v1.0-v1.3 public surface bound what this test
  // can assert, and are called out explicitly rather than silently worked
  // around:
  //
  //   Gap #1 (no write-to-allocation API): there is no public entry point to
  //   copy host bytes into an arenaAllocate()'d allocation anywhere in
  //   v1.0-v1.3. The source texture and vertex buffer built below are
  //   therefore necessarily zero-filled -- real geometry/pixel content can
  //   never be authored through the public ABI as it stands. This does not
  //   block the assertions below: Metal's raster pipeline is compiled from
  //   the submitted MSL text (and accepted or rejected based on its
  //   entry-point names) independently of vertex *content* -- draw-call
  //   vertex_count is derived purely from the vertex allocation's *byte
  //   size* (metal_device_hal.mm run_raster_pass/its caller), which is
  //   non-zero and a multiple of 3 here, so a zero-content vertex buffer
  //   still exercises the full pipeline-compile path this test cares about.
  //
  //   Gap #2 (no pixel-readback API): there is no public entry point to
  //   retrieve a submission's rendered pixels anywhere in v1.0-v1.3
  //   (hal::Submission::raster_results::resolved_rgba is never surfaced
  //   through VgApi; getSubmissionExecutionResult/getSubmissionLoweringReport
  //   both return outcome-metadata-only JSON). A direct "the output pixel is
  //   green" assertion is therefore impossible through the public ABI alone.
  //   In its place this test uses a differential proof that the actual
  //   submitted MSL text (not just "some MSL") drives real compilation
  //   behaviour: a well-formed fragment_entry name must submit with
  //   ok==true, and a fragment_entry name that does not exist in the MSL
  //   source must submit with ok==false and Metal's specific
  //   pipeline-compile-failure message -- proving the entry name the caller
  //   wrote into the JSON envelope is the one Metal actually tried to
  //   resolve, not just JSON that happened to parse.
  //
  // These are genuine gaps in the landed v1.0-v1.3 public API surface (not
  // bugs in this test), flagged here per this task's instructions rather
  // than worked around by touching include/vg/vg.h or src/api/*.cpp.
  {
    VgApi raster_api{};
    raster_api.size = sizeof(raster_api);
    if (!check(vgGetApi(VG_API_VERSION_1_6, &raster_api) == VG_SUCCESS, "vgGetApi v1.6")) return 1;
    check(raster_api.version == VG_API_VERSION_1_6, "raster_api.version == v1.6");
    check(raster_api.size == sizeof(VgApi), "raster_api.size matches the v1.6 table");

    VgRuntimeDesc raster_runtime_desc{};
    raster_runtime_desc.header.type = VG_STRUCTURE_RUNTIME_DESC;
    raster_runtime_desc.header.size = sizeof(raster_runtime_desc);
    VgRuntime raster_runtime = nullptr;
    if (!check(raster_api.createRuntime(&raster_runtime_desc, &raster_runtime) == VG_SUCCESS,
               "raster: createRuntime"))
      return 1;

    uint32_t raster_adapter_count = 0;
    check(raster_api.enumerateAdapters(raster_runtime, &raster_adapter_count, nullptr) == VG_SUCCESS,
          "raster: enumerateAdapters count");
    check(raster_adapter_count > 0, "raster: at least one adapter enumerated");
    std::vector<VgAdapterInfo> raster_adapters(raster_adapter_count);
    for (auto& info : raster_adapters) {
      info.header.type = VG_STRUCTURE_ADAPTER_INFO;
      info.header.size = sizeof(VgAdapterInfo);
    }
    uint32_t raster_adapter_written = raster_adapter_count;
    check(raster_api.enumerateAdapters(raster_runtime, &raster_adapter_written, raster_adapters.data()) ==
              VG_SUCCESS,
          "raster: enumerateAdapters fill");

    const VgAdapterInfo* raster_chosen = nullptr;
    for (const auto& info : raster_adapters) {
      if (info.backend_kind == VG_BACKEND_METAL) {
        raster_chosen = &info;
        break;
      }
    }
    if (raster_chosen == nullptr) {
      for (const auto& info : raster_adapters) {
        if (info.backend_kind == VG_BACKEND_REFERENCE) {
          raster_chosen = &info;
          break;
        }
      }
    }
    if (!check(raster_chosen != nullptr, "raster: a usable adapter (Metal or reference) is present")) return 1;
    const bool raster_is_metal = raster_chosen->backend_kind == VG_BACKEND_METAL;

    VgAdapter raster_adapter = nullptr;
    check(raster_api.openAdapter(raster_runtime, raster_chosen->stable_uuid, &raster_adapter) == VG_SUCCESS,
          "raster: openAdapter");

    VgDeviceDesc raster_device_desc{};
    raster_device_desc.header.type = VG_STRUCTURE_DEVICE_DESC;
    raster_device_desc.header.size = sizeof(raster_device_desc);
    VgDevice raster_device = nullptr;
    check(raster_api.createDevice(raster_adapter, &raster_device_desc, &raster_device) == VG_SUCCESS,
          "raster: createDevice");

    VgAddressDomainDesc raster_domain_desc{};
    raster_domain_desc.header.type = VG_STRUCTURE_ADDRESS_DOMAIN_DESC;
    raster_domain_desc.header.size = sizeof(raster_domain_desc);
    raster_domain_desc.kind = VG_ADDRESS_DOMAIN_DEVICE_LOCAL;
    VgAddressDomain raster_domain = nullptr;
    check(raster_api.createAddressDomain(raster_device, &raster_domain_desc, &raster_domain) == VG_SUCCESS,
          "raster: createAddressDomain");

    VgArenaDesc raster_arena_desc{};
    raster_arena_desc.header.type = VG_STRUCTURE_ARENA_DESC;
    raster_arena_desc.header.size = sizeof(raster_arena_desc);
    raster_arena_desc.domain = raster_domain;
    VgArena raster_arena = nullptr;
    check(raster_api.createArena(raster_device, &raster_arena_desc, &raster_arena) == VG_SUCCESS,
          "raster: createArena");

    // A 4x4 RGBA8Unorm source/target (64 bytes each) and a 6-vertex
    // (fullscreen-quad-shaped) vertex buffer (96 bytes). Gap #1 above means
    // these all stay zero-filled -- there is no public write path to load
    // the fullscreen-quad values metal_fullscreen_quad() uses internally
    // (tests/vertical_slice/metal_task_timeline_test.cpp).
    constexpr uint32_t kRasterExtent = 4;
    constexpr uint64_t kRasterTexelBytes = static_cast<uint64_t>(kRasterExtent) * kRasterExtent * 4;
    constexpr uint64_t kRasterVertexBytes = 6ull * 5ull * sizeof(float);  // 6 vertices * {x,y,z,u,v}

    VgAllocation raster_source_allocation = nullptr;
    check(raster_api.arenaAllocate(raster_arena, kRasterTexelBytes, &raster_source_allocation) == VG_SUCCESS,
          "raster: arenaAllocate source");
    uint64_t raster_source_id = 0;
    uint32_t raster_source_gen = 0;
    check(raster_api.getAllocationRef(raster_source_allocation, &raster_source_id, &raster_source_gen) ==
              VG_SUCCESS,
          "raster: getAllocationRef source");

    VgAllocation raster_target_allocation = nullptr;
    check(raster_api.arenaAllocate(raster_arena, kRasterTexelBytes, &raster_target_allocation) == VG_SUCCESS,
          "raster: arenaAllocate target");
    uint64_t raster_target_id = 0;
    uint32_t raster_target_gen = 0;
    check(raster_api.getAllocationRef(raster_target_allocation, &raster_target_id, &raster_target_gen) ==
              VG_SUCCESS,
          "raster: getAllocationRef target");

    VgAllocation raster_depth_allocation = nullptr;
    check(raster_api.arenaAllocate(raster_arena, kRasterTexelBytes, &raster_depth_allocation) == VG_SUCCESS,
          "raster: arenaAllocate depth");
    uint64_t raster_depth_id = 0;
    uint32_t raster_depth_gen = 0;
    check(raster_api.getAllocationRef(raster_depth_allocation, &raster_depth_id, &raster_depth_gen) == VG_SUCCESS,
          "raster: getAllocationRef depth");

    VgAllocation raster_vertex_allocation = nullptr;
    check(raster_api.arenaAllocate(raster_arena, kRasterVertexBytes, &raster_vertex_allocation) == VG_SUCCESS,
          "raster: arenaAllocate vertex buffer");
    uint64_t raster_vertex_id = 0;
    uint32_t raster_vertex_gen = 0;
    check(raster_api.getAllocationRef(raster_vertex_allocation, &raster_vertex_id, &raster_vertex_gen) ==
              VG_SUCCESS,
          "raster: getAllocationRef vertex buffer");

    VgAllocation raster_index16_allocation = nullptr;
    VgAllocation raster_index32_allocation = nullptr;
    check(raster_api.arenaAllocate(raster_arena, 6 * sizeof(uint16_t), &raster_index16_allocation) == VG_SUCCESS,
          "raster: arenaAllocate u16 index buffer");
    check(raster_api.arenaAllocate(raster_arena, 6 * sizeof(uint32_t), &raster_index32_allocation) == VG_SUCCESS,
          "raster: arenaAllocate u32 index buffer");
    uint64_t raster_index16_id = 0, raster_index32_id = 0;
    uint32_t raster_index16_gen = 0, raster_index32_gen = 0;
    check(raster_api.getAllocationRef(raster_index16_allocation, &raster_index16_id, &raster_index16_gen) == VG_SUCCESS,
          "raster: getAllocationRef u16 index buffer");
    check(raster_api.getAllocationRef(raster_index32_allocation, &raster_index32_id, &raster_index32_gen) == VG_SUCCESS,
          "raster: getAllocationRef u32 index buffer");

    // Checkpoint A: this translation unit includes only vg/vg.h, yet uploads
    // pixels/vertices/indices, submits an offscreen triangle-list, then reads
    // the color attachment through the public ABI.
    std::vector<uint8_t> uploaded_source(kRasterTexelBytes, 0);
    for (size_t texel = 0; texel < uploaded_source.size(); texel += 4) {
      uploaded_source[texel] = 255;
      uploaded_source[texel + 3] = 255;
    }
    const float uploaded_vertices[] = {
        -1.f, -1.f, .5f, 0.f, 1.f,  1.f, -1.f, .5f, 1.f, 1.f,
        -1.f,  1.f, .5f, 0.f, 0.f,  1.f,  1.f, .5f, 1.f, 0.f,
    };
    const uint16_t uploaded_indices[] = {0, 1, 2, 2, 1, 3};
    check(raster_api.writeAllocation(raster_arena, raster_source_allocation, 0,
                                     uploaded_source.data(), uploaded_source.size()) == VG_SUCCESS,
          "checkpoint-a: upload source pixels");
    check(raster_api.writeAllocation(raster_arena, raster_vertex_allocation, 0,
                                     uploaded_vertices, sizeof(uploaded_vertices)) == VG_SUCCESS,
          "checkpoint-a: upload vertices");
    check(raster_api.writeAllocation(raster_arena, raster_index16_allocation, 0,
                                     uploaded_indices, sizeof(uploaded_indices)) == VG_SUCCESS,
          "checkpoint-a: upload indices");

    // acquireFacet (the new v1.3 entry point): source is Sample-kind, target
    // is Attachment-kind, the vertex buffer is Address-kind -- mirroring
    // run_task_graph_raster_user_shader's internal-API precedent
    // (tests/vertical_slice/metal_task_timeline_test.cpp).
    VgCanonicalViewDesc raster_source_view{};
    raster_source_view.header.type = VG_STRUCTURE_CANONICAL_VIEW_DESC;
    raster_source_view.header.size = sizeof(raster_source_view);
    raster_source_view.allocation = raster_source_id;
    raster_source_view.allocation_generation = raster_source_gen;
    raster_source_view.format = VG_PIXEL_FORMAT_RGBA8_UNORM;
    raster_source_view.dimension = VG_VIEW_DIMENSION_TEXTURE_2D;
    raster_source_view.width = kRasterExtent;
    raster_source_view.height = kRasterExtent;
    raster_source_view.array_layers = 1;
    raster_source_view.mip_levels = 1;
    // VgCanonicalViewDesc has no default member initializers (plain C
    // struct), so zero-init leaves swizzle_red/green/blue/alpha all at 0
    // (VG_SWIZZLE_RED) -- a non-identity swizzle. The internal
    // core::SwizzleChannels default IS identity, so this must be set
    // explicitly or Metal rejects the Sample-kind facet with "Unsupported:
    // non-identity swizzle applies to SampleFacet only".
    raster_source_view.swizzle_red = VG_SWIZZLE_RED;
    raster_source_view.swizzle_green = VG_SWIZZLE_GREEN;
    raster_source_view.swizzle_blue = VG_SWIZZLE_BLUE;
    raster_source_view.swizzle_alpha = VG_SWIZZLE_ALPHA;
    VgFacetRef raster_source_facet{};
    check(raster_api.acquireFacet(raster_device, raster_arena, &raster_source_view, VG_FACET_KIND_SAMPLE,
                                   &raster_source_facet) == VG_SUCCESS,
          "raster: acquireFacet source");

    VgCanonicalViewDesc raster_target_view = raster_source_view;
    raster_target_view.allocation = raster_target_id;
    raster_target_view.allocation_generation = raster_target_gen;
    VgFacetRef raster_target_facet{};
    check(raster_api.acquireFacet(raster_device, raster_arena, &raster_target_view, VG_FACET_KIND_ATTACHMENT,
                                   &raster_target_facet) == VG_SUCCESS,
          "raster: acquireFacet target");

    VgCanonicalViewDesc raster_depth_view = raster_target_view;
    raster_depth_view.allocation = raster_depth_id;
    raster_depth_view.allocation_generation = raster_depth_gen;
    raster_depth_view.format = VG_PIXEL_FORMAT_DEPTH32_FLOAT;
    VgFacetRef raster_depth_facet{};
    check(raster_api.acquireFacet(raster_device, raster_arena, &raster_depth_view, VG_FACET_KIND_ATTACHMENT,
                                   &raster_depth_facet) == VG_SUCCESS,
          "raster: acquireFacet depth");

    VgCanonicalViewDesc raster_vertex_view{};
    raster_vertex_view.header.type = VG_STRUCTURE_CANONICAL_VIEW_DESC;
    raster_vertex_view.header.size = sizeof(raster_vertex_view);
    raster_vertex_view.allocation = raster_vertex_id;
    raster_vertex_view.allocation_generation = raster_vertex_gen;
    raster_vertex_view.format = VG_PIXEL_FORMAT_RGBA8_UNORM;
    raster_vertex_view.dimension = VG_VIEW_DIMENSION_TEXTURE_2D;
    raster_vertex_view.width = static_cast<uint32_t>(kRasterVertexBytes / 4);
    raster_vertex_view.height = 1;
    raster_vertex_view.array_layers = 1;
    raster_vertex_view.mip_levels = 1;
    raster_vertex_view.swizzle_red = VG_SWIZZLE_RED;
    raster_vertex_view.swizzle_green = VG_SWIZZLE_GREEN;
    raster_vertex_view.swizzle_blue = VG_SWIZZLE_BLUE;
    raster_vertex_view.swizzle_alpha = VG_SWIZZLE_ALPHA;
    VgFacetRef raster_vertex_facet{};
    check(raster_api.acquireFacet(raster_device, raster_arena, &raster_vertex_view, VG_FACET_KIND_ADDRESS,
                                   &raster_vertex_facet) == VG_SUCCESS,
          "raster: acquireFacet vertex buffer");

    VgCanonicalViewDesc raster_index16_view = raster_vertex_view;
    raster_index16_view.allocation = raster_index16_id;
    raster_index16_view.allocation_generation = raster_index16_gen;
    raster_index16_view.format = VG_PIXEL_FORMAT_R16_UINT;
    raster_index16_view.width = 6;
    VgCanonicalViewDesc raster_index32_view = raster_index16_view;
    raster_index32_view.allocation = raster_index32_id;
    raster_index32_view.allocation_generation = raster_index32_gen;
    raster_index32_view.format = VG_PIXEL_FORMAT_R32_UINT;
    VgFacetRef raster_index16_facet{}, raster_index32_facet{};
    check(raster_api.acquireFacet(raster_device, raster_arena, &raster_index16_view, VG_FACET_KIND_ADDRESS,
                                  &raster_index16_facet) == VG_SUCCESS,
          "raster: acquireFacet u16 index Address");
    check(raster_api.acquireFacet(raster_device, raster_arena, &raster_index32_view, VG_FACET_KIND_ADDRESS,
                                  &raster_index32_facet) == VG_SUCCESS,
          "raster: acquireFacet u32 index Address");
    VgFacetRef rejected_index_facet{};
    check(raster_api.acquireFacet(raster_device, raster_arena, &raster_index16_view, VG_FACET_KIND_SAMPLE,
                                  &rejected_index_facet) == VG_ERROR_INVALID_ARGUMENT,
          "raster: R16Uint rejects Sample facet");
    check(raster_api.acquireFacet(raster_device, raster_arena, &raster_index32_view, VG_FACET_KIND_ATTACHMENT,
                                  &rejected_index_facet) == VG_ERROR_INVALID_ARGUMENT,
          "raster: R32Uint rejects Attachment facet");

    // A restricted-import MSL raster shader, structurally matching the fixed
    // binding contract the encoder unconditionally uses -- buffer/texture/
    // sampler index 0 for the vertex buffer/tint buffer/texture/sampler
    // respectively (src/compiler/compiler.h's kRasterVertexBufferIndex,
    // kRasterTintBufferIndex, kRasterTextureIndex, kRasterSamplerIndex, all
    // literally 0; mirrored here as literals since this test may not include
    // internal headers). Mirrors user_raster_msl_source() in
    // tests/vertical_slice/metal_task_timeline_test.cpp, but with locally
    // chosen entry-point names.
    const std::string raster_vertex_entry = "vg_c_abi_raster_vertex";
    const std::string raster_good_fragment_entry = "vg_c_abi_raster_fragment";
    const auto make_raster_msl_source = [&](const std::string& fragment_entry) {
      return std::string(
                 "#include <metal_stdlib>\n"
                 "using namespace metal;\n\n"
                 "struct VgRasterVertex { packed_float3 position; packed_float2 uv; };\n"
                 "struct VgRasterVaryings { float4 position [[position]]; float2 uv; };\n"
                 "struct VgRasterFragment { float4 color [[color(0)]]; };\n\n"
                 "vertex VgRasterVaryings ") +
             raster_vertex_entry +
             "(device const VgRasterVertex* vertices [[buffer(0)]],\n"
             "                                         uint vid [[vertex_id]]) {\n"
             "  VgRasterVaryings varyings;\n"
             "  varyings.position = float4(float3(vertices[vid].position), 1.0f);\n"
             "  varyings.uv = float2(vertices[vid].uv);\n"
             "  return varyings;\n"
             "}\n\n"
             "fragment VgRasterFragment " +
             fragment_entry +
             "(VgRasterVaryings varyings [[stage_in]],\n"
             "                                             texture2d<float, access::sample> tex "
             "[[texture(0)]],\n"
             "                                             sampler samp [[sampler(0)]],\n"
             "                                             constant float4& tint [[buffer(0)]]) {\n"
             "  VgRasterFragment result;\n"
             "  result.color = tex.sample(samp, varyings.uv) * tint;\n"
             "  return result;\n"
             "}\n";
    };

    // Minimal JSON-string escaper for embedding the MSL source (which
    // contains newlines) inside the msl-raster envelope's "source" field.
    const auto json_escape = [](const std::string& text) {
      std::string out;
      out.reserve(text.size());
      for (const char c : text) {
        switch (c) {
          case '"':
            out += "\\\"";
            break;
          case '\\':
            out += "\\\\";
            break;
          case '\n':
            out += "\\n";
            break;
          case '\r':
            out += "\\r";
            break;
          case '\t':
            out += "\\t";
            break;
          default:
            out += c;
            break;
        }
      }
      return out;
    };

    // The F4 "vg.msl.raster/v1" envelope requires an exact vertex_abi as
    // well as root_schema, vertex_entry, fragment_entry, and source.
    //
    // declared_fragment_entry is the envelope's fragment_entry field (the
    // name Metal looks up via newFunctionWithName: at ensure_raster_pipeline
    // time -- metal_device_hal.mm). source_fragment_entry is the name the
    // MSL source text actually *defines* its fragment function under. These
    // must be independent parameters: the malformed-entry negative case
    // needs a declared name that is genuinely absent from the compiled
    // library, not merely a different string baked into both places (which
    // would make the source define exactly the function being looked up,
    // defeating the negative test).
    const auto make_raster_envelope_json = [&](const std::string& declared_fragment_entry,
                                                const std::string& source_fragment_entry) {
      return "{\"root_schema\":\"vg.c-abi-conformance.raster/v1\",\"vertex_entry\":\"" + raster_vertex_entry +
             "\",\"fragment_entry\":\"" + declared_fragment_entry +
             "\",\"vertex_abi\":\"vg.raster.vertex.xyzuv-packed/v1\",\"source\":\"" +
             json_escape(make_raster_msl_source(source_fragment_entry)) + "\"}";
    };

    // ---- (a) Happy path: a well-formed custom raster shader submits
    // successfully through the full public C-ABI raster path. ----
    const std::string raster_good_envelope_json =
        make_raster_envelope_json(raster_good_fragment_entry, raster_good_fragment_entry);

    VgCodeObjectDesc raster_code_desc{};
    raster_code_desc.header.type = VG_STRUCTURE_CODE_OBJECT_DESC;
    raster_code_desc.header.size = sizeof(raster_code_desc);
    raster_code_desc.bytes = raster_good_envelope_json.data();
    raster_code_desc.byte_size = raster_good_envelope_json.size();
    raster_code_desc.format_tag = "vg.msl.raster/v1";
    VgCodeObject raster_code_object = nullptr;
    check(raster_api.loadCodeObject(raster_device, &raster_code_desc, &raster_code_object) == VG_SUCCESS,
          "raster: loadCodeObject vg.msl.raster/v1");

    // taskGraphAppend validates the caller-supplied VgNodeRef unconditionally
    // regardless of task kind (vg_api_taskgraph.cpp), so a raster-only task
    // graph still needs a throwaway node -- createNode is format-tag-agnostic
    // and never parses the MSL source (vg_api_code.cpp).
    VgNodeDesc raster_node_desc{};
    raster_node_desc.header.type = VG_STRUCTURE_NODE_DESC;
    raster_node_desc.header.size = sizeof(raster_node_desc);
    raster_node_desc.entry_name = "vg_c_abi_raster_throwaway_node";
    VgNode raster_node = nullptr;
    check(raster_api.createNode(raster_code_object, &raster_node_desc, &raster_node) == VG_SUCCESS,
          "raster: createNode");
    VgNodeRef raster_node_ref{};
    check(raster_api.getNodeRef(raster_node, &raster_node_ref) == VG_SUCCESS, "raster: getNodeRef");

    VgTaskGraphBuilderDesc raster_builder_desc{};
    raster_builder_desc.header.type = VG_STRUCTURE_TASK_GRAPH_BUILDER_DESC;
    raster_builder_desc.header.size = sizeof(raster_builder_desc);
    raster_builder_desc.code_object = raster_code_object;
    VgTaskGraphBuilder raster_builder = nullptr;
    check(raster_api.createTaskGraphBuilder(raster_device, &raster_builder_desc, &raster_builder) == VG_SUCCESS,
          "raster: createTaskGraphBuilder");

    VgTaskRecordV2 raster_task{};
    raster_task.node = raster_node_ref;
    raster_task.root = 0;
    raster_task.root_generation = 1;  // Non-zero; unused by a raster task, but
                                       // TaskGraphBuilder::append (device_hal.cpp)
                                       // requires node_generation/root_generation != 0
                                       // regardless of task kind.
    raster_task.shape = {1, 1, 1, 0};
    raster_task.kind = VG_TASK_KIND_RASTER;
    raster_task.topology = VG_TOPOLOGY_TRIANGLE_LIST;
    raster_task.raster_facets.source = raster_source_facet;
    raster_task.raster_facets.target = raster_target_facet;
    raster_task.vertex_buffer_ref = raster_vertex_facet;
    raster_task.index_buffer_ref = raster_index16_facet;
    raster_task.index_count = 6;
    raster_task.raster_filter = VG_FILTER_BILINEAR;
    raster_task.raster_wrap = VG_WRAP_CLAMP;
    raster_task.raster_tint[0] = 1.0f;
    raster_task.raster_tint[1] = 1.0f;
    raster_task.raster_tint[2] = 1.0f;
    raster_task.raster_tint[3] = 1.0f;
    raster_task.depth_attachment_ref = raster_depth_facet;
    raster_task.depth_test_enable = VG_TRUE;
    raster_task.depth_write_enable = VG_TRUE;
    raster_task.depth_compare_op = VG_DEPTH_COMPARE_LESS;

    VgTaskId raster_task_id{};
    check(raster_api.taskGraphAppendV2(raster_builder, &raster_task, 1, &raster_task_id) == VG_SUCCESS,
          "raster: taskGraphAppendV2 depth-enabled VG_TASK_KIND_RASTER");

    VgSealDesc raster_seal_desc{};
    raster_seal_desc.header.type = VG_STRUCTURE_SEAL_DESC;
    raster_seal_desc.header.size = sizeof(raster_seal_desc);
    VgTaskGraph raster_graph = nullptr;
    check(raster_api.sealTaskGraph(raster_builder, &raster_seal_desc, &raster_graph) == VG_SUCCESS,
          "raster: sealTaskGraph");

    VgExecutionEnvelopeDesc raster_envelope_desc{};
    raster_envelope_desc.header.type = VG_STRUCTURE_EXECUTION_ENVELOPE_DESC;
    raster_envelope_desc.header.size = sizeof(raster_envelope_desc);
    raster_envelope_desc.arena = raster_arena;
    VgExecutionEnvelope raster_envelope = nullptr;
    check(raster_api.createExecutionEnvelope(raster_device, &raster_envelope_desc, &raster_envelope) ==
              VG_SUCCESS,
          "raster: createExecutionEnvelope");

    VgTimeline raster_timeline = nullptr;
    check(raster_api.createTimeline(raster_device, &raster_timeline) == VG_SUCCESS, "raster: createTimeline");

    VgSubmitDesc raster_submit_desc{};
    raster_submit_desc.header.type = VG_STRUCTURE_SUBMIT_DESC;
    raster_submit_desc.header.size = sizeof(raster_submit_desc);
    raster_submit_desc.graph = raster_graph;
    raster_submit_desc.envelope = raster_envelope;
    VgSubmission raster_submission = nullptr;
    check(raster_api.submit(raster_device, &raster_submit_desc, &raster_submission) == VG_SUCCESS,
          "raster: submit (well-formed MSL)");

    const char* raster_result_json = nullptr;
    check(raster_api.getSubmissionExecutionResult(raster_submission, &raster_result_json) == VG_SUCCESS,
          "raster: getSubmissionExecutionResult (well-formed MSL)");
    const std::string raster_result_str = raster_result_json != nullptr ? raster_result_json : "";
    // core::ExecutionResult::canonical_json() serializes `ok` as the integer
    // 0/1 (core.cpp), not a JSON boolean literal.
    check(raster_result_str.find("\"ok\":1") != std::string::npos,
          "raster: well-formed MSL submission execution result reports ok:1 (true)");
    std::vector<uint8_t> checkpoint_pixels(kRasterTexelBytes);
    check(raster_api.readAllocation(raster_arena, raster_target_allocation, 0,
                                    checkpoint_pixels.data(), checkpoint_pixels.size()) == VG_SUCCESS &&
              checkpoint_pixels[(kRasterExtent + 1) * 4] == 255 &&
              checkpoint_pixels[(kRasterExtent + 1) * 4 + 1] == 0 &&
              checkpoint_pixels[(kRasterExtent + 1) * 4 + 3] == 255,
          "checkpoint-a: public readback observes uploaded offscreen texture pixel");

    raster_api.destroySubmission(raster_submission);
    raster_api.destroyTimeline(raster_timeline);
    raster_api.destroyExecutionEnvelope(raster_envelope);
    raster_api.destroyTaskGraph(raster_graph);
    raster_api.destroyTaskGraphBuilder(raster_builder);
    raster_api.destroyNode(raster_node);
    raster_api.destroyCodeObject(raster_code_object);

    // ---- (b) Metal-only negative: fragment_entry names a function absent
    // from the submitted MSL source. Reference never interprets MSL text
    // (ADR-018 -- it always runs a fixed C++ shading formula regardless of
    // entry-name correctness, reference_device_hal.cpp), so this sub-case
    // only makes sense on Metal. Metal's raster pipeline compiles lazily at
    // submit() time (ensure_raster_pipeline in metal_device_hal.mm), so
    // submit() itself still returns VG_SUCCESS (host-side acceptance) but the
    // execution result reports ok:false with the specific
    // pipeline-compile-failure message -- proof the actual entry name string
    // drives real Metal shader compilation, not just JSON parsing. ----
    if (raster_is_metal) {
      const std::string raster_bad_fragment_entry = "vg_c_abi_raster_fragment_does_not_exist";
      // make_raster_msl_source(raster_good_fragment_entry) means the compiled
      // MSL source only defines a fragment function named
      // raster_good_fragment_entry; declaring fragment_entry as
      // raster_bad_fragment_entry means Metal's newFunctionWithName: lookup
      // at ensure_raster_pipeline time genuinely fails to find it -- not
      // just a mismatched JSON field with a matching function baked in
      // alongside it.
      const std::string raster_bad_envelope_json =
          make_raster_envelope_json(raster_bad_fragment_entry, raster_good_fragment_entry);

      VgCodeObjectDesc raster_bad_code_desc{};
      raster_bad_code_desc.header.type = VG_STRUCTURE_CODE_OBJECT_DESC;
      raster_bad_code_desc.header.size = sizeof(raster_bad_code_desc);
      raster_bad_code_desc.bytes = raster_bad_envelope_json.data();
      raster_bad_code_desc.byte_size = raster_bad_envelope_json.size();
      raster_bad_code_desc.format_tag = "vg.msl.raster/v1";
      VgCodeObject raster_bad_code_object = nullptr;
      check(raster_api.loadCodeObject(raster_device, &raster_bad_code_desc, &raster_bad_code_object) ==
                VG_SUCCESS,
            "raster: loadCodeObject (malformed fragment_entry setup)");

      VgNodeDesc raster_bad_node_desc{};
      raster_bad_node_desc.header.type = VG_STRUCTURE_NODE_DESC;
      raster_bad_node_desc.header.size = sizeof(raster_bad_node_desc);
      raster_bad_node_desc.entry_name = "vg_c_abi_raster_bad_throwaway_node";
      VgNode raster_bad_node = nullptr;
      check(raster_api.createNode(raster_bad_code_object, &raster_bad_node_desc, &raster_bad_node) == VG_SUCCESS,
            "raster: createNode (malformed fragment_entry setup)");
      VgNodeRef raster_bad_node_ref{};
      check(raster_api.getNodeRef(raster_bad_node, &raster_bad_node_ref) == VG_SUCCESS,
            "raster: getNodeRef (malformed fragment_entry setup)");

      VgTaskGraphBuilderDesc raster_bad_builder_desc{};
      raster_bad_builder_desc.header.type = VG_STRUCTURE_TASK_GRAPH_BUILDER_DESC;
      raster_bad_builder_desc.header.size = sizeof(raster_bad_builder_desc);
      raster_bad_builder_desc.code_object = raster_bad_code_object;
      VgTaskGraphBuilder raster_bad_builder = nullptr;
      check(raster_api.createTaskGraphBuilder(raster_device, &raster_bad_builder_desc, &raster_bad_builder) ==
                VG_SUCCESS,
            "raster: createTaskGraphBuilder (malformed fragment_entry setup)");

      VgTaskRecord raster_bad_task{};
      raster_bad_task.node = raster_bad_node_ref;
      raster_bad_task.root = 0;
      raster_bad_task.root_generation = 1;
      raster_bad_task.shape = {1, 1, 1, 0};
      raster_bad_task.kind = VG_TASK_KIND_RASTER;
      raster_bad_task.topology = VG_TOPOLOGY_TRIANGLE_LIST;
      raster_bad_task.raster_facets.source = raster_source_facet;
      raster_bad_task.raster_facets.target = raster_target_facet;
      raster_bad_task.vertex_buffer_ref = raster_vertex_facet;
      raster_bad_task.raster_filter = VG_FILTER_BILINEAR;
      raster_bad_task.raster_wrap = VG_WRAP_CLAMP;
      raster_bad_task.raster_tint[0] = 1.0f;
      raster_bad_task.raster_tint[1] = 1.0f;
      raster_bad_task.raster_tint[2] = 1.0f;
      raster_bad_task.raster_tint[3] = 1.0f;

      VgTaskId raster_bad_task_id{};
      check(raster_api.taskGraphAppend(raster_bad_builder, &raster_bad_task, 1, &raster_bad_task_id) ==
                VG_SUCCESS,
            "raster: taskGraphAppend (malformed fragment_entry setup)");

      VgSealDesc raster_bad_seal_desc{};
      raster_bad_seal_desc.header.type = VG_STRUCTURE_SEAL_DESC;
      raster_bad_seal_desc.header.size = sizeof(raster_bad_seal_desc);
      VgTaskGraph raster_bad_graph = nullptr;
      check(raster_api.sealTaskGraph(raster_bad_builder, &raster_bad_seal_desc, &raster_bad_graph) == VG_SUCCESS,
            "raster: sealTaskGraph (malformed fragment_entry setup)");

      VgExecutionEnvelopeDesc raster_bad_envelope_desc{};
      raster_bad_envelope_desc.header.type = VG_STRUCTURE_EXECUTION_ENVELOPE_DESC;
      raster_bad_envelope_desc.header.size = sizeof(raster_bad_envelope_desc);
      raster_bad_envelope_desc.arena = raster_arena;
      VgExecutionEnvelope raster_bad_envelope = nullptr;
      check(raster_api.createExecutionEnvelope(raster_device, &raster_bad_envelope_desc, &raster_bad_envelope) ==
                VG_SUCCESS,
            "raster: createExecutionEnvelope (malformed fragment_entry setup)");

      VgTimeline raster_bad_timeline = nullptr;
      check(raster_api.createTimeline(raster_device, &raster_bad_timeline) == VG_SUCCESS,
            "raster: createTimeline (malformed fragment_entry setup)");

      VgSubmitDesc raster_bad_submit_desc{};
      raster_bad_submit_desc.header.type = VG_STRUCTURE_SUBMIT_DESC;
      raster_bad_submit_desc.header.size = sizeof(raster_bad_submit_desc);
      raster_bad_submit_desc.graph = raster_bad_graph;
      raster_bad_submit_desc.envelope = raster_bad_envelope;
      VgSubmission raster_bad_submission = nullptr;
      check(raster_api.submit(raster_device, &raster_bad_submit_desc, &raster_bad_submission) == VG_SUCCESS,
            "raster: submit (malformed fragment_entry) still returns VG_SUCCESS (host-side acceptance)");

      const char* raster_bad_result_json = nullptr;
      check(raster_api.getSubmissionExecutionResult(raster_bad_submission, &raster_bad_result_json) ==
                VG_SUCCESS,
            "raster: getSubmissionExecutionResult (malformed fragment_entry)");
      const std::string raster_bad_result_str =
          raster_bad_result_json != nullptr ? raster_bad_result_json : "";
      check(raster_bad_result_str.find("\"ok\":0") != std::string::npos,
            "raster: malformed fragment_entry submission execution result reports ok:0 (false)");
      check(raster_bad_result_str.find("Metal raster pipeline compile failed") != std::string::npos,
            "raster: malformed fragment_entry execution result names the Metal pipeline-compile failure");

      raster_api.destroySubmission(raster_bad_submission);
      raster_api.destroyTimeline(raster_bad_timeline);
      raster_api.destroyExecutionEnvelope(raster_bad_envelope);
      raster_api.destroyTaskGraph(raster_bad_graph);
      raster_api.destroyTaskGraphBuilder(raster_bad_builder);
      raster_api.destroyNode(raster_bad_node);
      raster_api.destroyCodeObject(raster_bad_code_object);
    }

    // ---- (c) Backend-agnostic negative: a "vg.msl.raster/v1" submission may
    // only contain raster tasks -- device_hal.cpp's ExecutionPlan::validate()
    // rejects any mix of compute+raster under a user_raster_shader plan,
    // independent of backend. ----
    {
      const std::string raster_mixed_envelope_json =
          make_raster_envelope_json(raster_good_fragment_entry, raster_good_fragment_entry);

      VgCodeObjectDesc raster_mixed_code_desc{};
      raster_mixed_code_desc.header.type = VG_STRUCTURE_CODE_OBJECT_DESC;
      raster_mixed_code_desc.header.size = sizeof(raster_mixed_code_desc);
      raster_mixed_code_desc.bytes = raster_mixed_envelope_json.data();
      raster_mixed_code_desc.byte_size = raster_mixed_envelope_json.size();
      raster_mixed_code_desc.format_tag = "vg.msl.raster/v1";
      VgCodeObject raster_mixed_code_object = nullptr;
      check(raster_api.loadCodeObject(raster_device, &raster_mixed_code_desc, &raster_mixed_code_object) ==
                VG_SUCCESS,
            "raster: loadCodeObject (mixed compute+raster setup)");

      VgNodeDesc raster_mixed_node_desc{};
      raster_mixed_node_desc.header.type = VG_STRUCTURE_NODE_DESC;
      raster_mixed_node_desc.header.size = sizeof(raster_mixed_node_desc);
      raster_mixed_node_desc.entry_name = "vg_c_abi_raster_mixed_throwaway_node";
      VgNode raster_mixed_node = nullptr;
      check(raster_api.createNode(raster_mixed_code_object, &raster_mixed_node_desc, &raster_mixed_node) ==
                VG_SUCCESS,
            "raster: createNode (mixed compute+raster setup)");
      VgNodeRef raster_mixed_node_ref{};
      check(raster_api.getNodeRef(raster_mixed_node, &raster_mixed_node_ref) == VG_SUCCESS,
            "raster: getNodeRef (mixed compute+raster setup)");

      VgTaskGraphBuilderDesc raster_mixed_builder_desc{};
      raster_mixed_builder_desc.header.type = VG_STRUCTURE_TASK_GRAPH_BUILDER_DESC;
      raster_mixed_builder_desc.header.size = sizeof(raster_mixed_builder_desc);
      raster_mixed_builder_desc.code_object = raster_mixed_code_object;
      VgTaskGraphBuilder raster_mixed_builder = nullptr;
      check(raster_api.createTaskGraphBuilder(raster_device, &raster_mixed_builder_desc,
                                               &raster_mixed_builder) == VG_SUCCESS,
            "raster: createTaskGraphBuilder (mixed compute+raster setup)");

      VgTaskRecord raster_mixed_tasks[2]{};
      raster_mixed_tasks[0].node = raster_mixed_node_ref;
      raster_mixed_tasks[0].root = 0;
      raster_mixed_tasks[0].root_generation = 1;
      raster_mixed_tasks[0].shape = {1, 1, 1, 0};
      raster_mixed_tasks[0].kind = VG_TASK_KIND_COMPUTE;

      raster_mixed_tasks[1].node = raster_mixed_node_ref;
      raster_mixed_tasks[1].root = 0;
      raster_mixed_tasks[1].root_generation = 1;
      raster_mixed_tasks[1].shape = {1, 1, 1, 0};
      raster_mixed_tasks[1].kind = VG_TASK_KIND_RASTER;
      raster_mixed_tasks[1].topology = VG_TOPOLOGY_TRIANGLE_LIST;
      raster_mixed_tasks[1].raster_facets.source = raster_source_facet;
      raster_mixed_tasks[1].raster_facets.target = raster_target_facet;
      raster_mixed_tasks[1].vertex_buffer_ref = raster_vertex_facet;
      raster_mixed_tasks[1].raster_filter = VG_FILTER_BILINEAR;
      raster_mixed_tasks[1].raster_wrap = VG_WRAP_CLAMP;
      raster_mixed_tasks[1].raster_tint[0] = 1.0f;
      raster_mixed_tasks[1].raster_tint[1] = 1.0f;
      raster_mixed_tasks[1].raster_tint[2] = 1.0f;
      raster_mixed_tasks[1].raster_tint[3] = 1.0f;

      VgTaskId raster_mixed_ids[2]{};
      check(raster_api.taskGraphAppend(raster_mixed_builder, raster_mixed_tasks, 2, raster_mixed_ids) ==
                VG_SUCCESS,
            "raster: taskGraphAppend mixed compute+raster (mixed compute+raster setup)");

      VgSealDesc raster_mixed_seal_desc{};
      raster_mixed_seal_desc.header.type = VG_STRUCTURE_SEAL_DESC;
      raster_mixed_seal_desc.header.size = sizeof(raster_mixed_seal_desc);
      VgTaskGraph raster_mixed_graph = nullptr;
      check(raster_api.sealTaskGraph(raster_mixed_builder, &raster_mixed_seal_desc, &raster_mixed_graph) ==
                VG_SUCCESS,
            "raster: sealTaskGraph (mixed compute+raster setup)");

      VgExecutionEnvelopeDesc raster_mixed_envelope_desc{};
      raster_mixed_envelope_desc.header.type = VG_STRUCTURE_EXECUTION_ENVELOPE_DESC;
      raster_mixed_envelope_desc.header.size = sizeof(raster_mixed_envelope_desc);
      raster_mixed_envelope_desc.arena = raster_arena;
      VgExecutionEnvelope raster_mixed_envelope = nullptr;
      check(raster_api.createExecutionEnvelope(raster_device, &raster_mixed_envelope_desc,
                                                &raster_mixed_envelope) == VG_SUCCESS,
            "raster: createExecutionEnvelope (mixed compute+raster setup)");

      VgTimeline raster_mixed_timeline = nullptr;
      check(raster_api.createTimeline(raster_device, &raster_mixed_timeline) == VG_SUCCESS,
            "raster: createTimeline (mixed compute+raster setup)");

      VgSubmitDesc raster_mixed_submit_desc{};
      raster_mixed_submit_desc.header.type = VG_STRUCTURE_SUBMIT_DESC;
      raster_mixed_submit_desc.header.size = sizeof(raster_mixed_submit_desc);
      raster_mixed_submit_desc.graph = raster_mixed_graph;
      raster_mixed_submit_desc.envelope = raster_mixed_envelope;
      VgSubmission raster_mixed_submission = nullptr;
      check(raster_api.submit(raster_device, &raster_mixed_submit_desc, &raster_mixed_submission) ==
                VG_ERROR_INVALID_ARGUMENT,
            "raster: submit rejects a mixed compute+raster user_raster_shader submission (backend-agnostic)");

      raster_api.destroyExecutionEnvelope(raster_mixed_envelope);
      raster_api.destroyTimeline(raster_mixed_timeline);
      raster_api.destroyTaskGraph(raster_mixed_graph);
      raster_api.destroyTaskGraphBuilder(raster_mixed_builder);
      raster_api.destroyNode(raster_mixed_node);
      raster_api.destroyCodeObject(raster_mixed_code_object);
    }

    raster_api.destroyArena(raster_arena);
    raster_api.destroyAddressDomain(raster_domain);
    raster_api.destroyDevice(raster_device);
    raster_api.closeAdapter(raster_adapter);
    raster_api.destroyRuntime(raster_runtime);
  }

  // Version skew (Sec.16), v1.3 extension: request v1.2 from a v1.3-capable
  // library -- the v1.2/v1.3 boundary is offsetof(VgApi, acquireFacet),
  // mirroring the v1.0/v1.1 and v1.1/v1.2 boundary checks above.
  {
    VgApi v12_api{};
    v12_api.size = sizeof(v12_api);
    check(vgGetApi(VG_API_VERSION_1_2, &v12_api) == VG_SUCCESS, "vgGetApi v1.2");
    check(v12_api.version == VG_API_VERSION_1_2, "v1.2 api.version");
    check(v12_api.size == offsetof(VgApi, acquireFacet), "v1.2 api.size matches the v1.2/v1.3 boundary");

    VgRuntimeDesc desc{};
    desc.header.type = VG_STRUCTURE_RUNTIME_DESC;
    desc.header.size = sizeof(desc);
    VgRuntime rt = nullptr;
    check(v12_api.createRuntime(&desc, &rt) == VG_SUCCESS, "v1.2 createRuntime");
    uint32_t count = 0;
    check(v12_api.enumerateAdapters(rt, &count, nullptr) == VG_SUCCESS, "v1.2 enumerateAdapters");
    v12_api.destroyRuntime(rt);
  }

  // v1.4 appends only the V2 raw-record entry point. Its v1.3 predecessor
  // must remain a complete, correctly sized table rather than receiving a
  // pointer beyond its negotiated boundary.
  {
    VgApi v14_api{};
    v14_api.size = sizeof(v14_api);
    check(vgGetApi(VG_API_VERSION_1_4, &v14_api) == VG_SUCCESS, "vgGetApi v1.4");
    check(v14_api.version == VG_API_VERSION_1_4, "v1.4 api.version");
    check(v14_api.size == offsetof(VgApi, writeAllocation), "v1.4 api.size == v1.6 boundary");
    check(v14_api.taskGraphAppendV2 != nullptr, "v1.4 taskGraphAppendV2 is populated");
  }

  // F5 activates indexed raster through the existing V2 record and API-table
  // member. v1.5 therefore negotiates the same table boundary as v1.4.
  {
    VgApi v15_api{};
    v15_api.size = sizeof(v15_api);
    check(vgGetApi(VG_API_VERSION_1_5, &v15_api) == VG_SUCCESS, "vgGetApi v1.5");
    check(v15_api.version == VG_API_VERSION_1_5, "v1.5 api.version");
    check(v15_api.size == offsetof(VgApi, writeAllocation), "v1.5 api.size == v1.6 boundary");
    check(v15_api.taskGraphAppendV2 != nullptr, "v1.5 reuses taskGraphAppendV2");
  }

  return g_ok ? 0 : 1;
}
