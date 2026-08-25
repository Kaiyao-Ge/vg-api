// F1 (ADR-043 Decision #2 / ADR-044): the actual Checkpoint-A precondition.
// Includes only <vg/vg.h> and links only vg_api -- proves the full v1.1
// golden path, stale-handle rejection, and version skew are all reachable
// through the public C ABI alone, never touching vg_core/vg_backend_reference
// directly.
#include "vg/vg.h"

#include <cstddef>
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

  return g_ok ? 0 : 1;
}
