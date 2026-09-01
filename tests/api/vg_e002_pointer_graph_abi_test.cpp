// Reruns E002 (typed pointer graph, ADR-028/TASK-B15) through the public C
// ABI alone (<vg/vg.h>, linking only vg_api) and checks the result against
// what tests/vertical_slice/metal_task_timeline_test.cpp's run_pointer_graph
// already verified on real Metal hardware:
//   1. compile() classifies the NodeRef-keyed "node_compute_package" event CachedObject
//      (not Direct) for a module carrying declared_pointer_edges -- this is
//      driven by the Task's resolved immutable Node module, reachable through
//      loadCodeObject's IR-JSON text even though the public ABI has no notion
//      of PointerEdge as a distinct object.
//   2. The full golden path (openAdapter..submit) completes with VG_SUCCESS
//      through a real, uuid-selected Metal device.
//
// Known, disclosed gap this test surfaces rather than works around: the
// internal experiment also reads target.bytes back to confirm the store_via
// byte-broadcast pattern (all 42). There is no host-visible-memory read-back
// entry point in vg.h, so this test cannot independently confirm the GPU-side
// write actually landed. core::Submission::result.ok, previously in the same
// unreachable category, became reachable via v1.2's getSubmissionExecutionResult
// (ADR-045) and is now checked below.
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
  if (!check(vgGetApi(VG_API_VERSION_1_2, &api) == VG_SUCCESS, "vgGetApi v1.2")) return 1;

  VgRuntimeDesc runtime_desc{};
  runtime_desc.header.type = VG_STRUCTURE_RUNTIME_DESC;
  runtime_desc.header.size = sizeof(runtime_desc);
  VgRuntime runtime = nullptr;
  if (!check(api.createRuntime(&runtime_desc, &runtime) == VG_SUCCESS, "createRuntime")) return 1;

  uint32_t adapter_count = 0;
  check(api.enumerateAdapters(runtime, &adapter_count, nullptr) == VG_SUCCESS, "enumerateAdapters count");
  std::vector<VgAdapterInfo> adapters(adapter_count);
  for (auto& info : adapters) {
    info.header.type = VG_STRUCTURE_ADAPTER_INFO;
    info.header.size = sizeof(VgAdapterInfo);
  }
  uint32_t written = adapter_count;
  check(api.enumerateAdapters(runtime, &written, adapters.data()) == VG_SUCCESS, "enumerateAdapters fill");

  // This case specifically measures Metal's native CachedObject-vs-Direct
  // package classification. Reference executes the same bounded pointer
  // graph semantically, while Vulkan returns an explicit Unsupported report.
  const VgAdapterInfo* chosen = nullptr;
  for (const auto& info : adapters) {
    if (info.backend_kind == VG_BACKEND_METAL) {
      chosen = &info;
      break;
    }
  }
  if (chosen == nullptr) {
    std::fprintf(stderr, "e002-pointer-graph-via-abi: no Metal adapter enumerated on this host, skipping\n");
    api.destroyRuntime(runtime);
    return 0;
  }

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

  // Mirrors run_pointer_graph(): allocation A (16 bytes) holds the load_ref
  // never dereferenced on the GPU; allocation B (4 bytes) is the store_via
  // target statically resolved through declared_pointer_edges.
  VgAllocation ref_holder = nullptr;
  check(api.arenaAllocate(arena, 16, &ref_holder) == VG_SUCCESS, "arenaAllocate ref_holder");
  uint64_t ref_id = 0;
  uint32_t ref_generation = 0;
  check(api.getAllocationRef(ref_holder, &ref_id, &ref_generation) == VG_SUCCESS, "getAllocationRef ref_holder");

  VgAllocation target = nullptr;
  check(api.arenaAllocate(arena, 4, &target) == VG_SUCCESS, "arenaAllocate target");
  uint64_t target_id = 0;
  uint32_t target_generation = 0;
  check(api.getAllocationRef(target, &target_id, &target_generation) == VG_SUCCESS, "getAllocationRef target");

  // A freshly allocated Allocation's representation_epoch is 0 (core.h:31);
  // that is not surfaced by getAllocationRef, so it is asserted here rather
  // than queried -- exactly the value run_pointer_graph() reads directly off
  // the C++ Allocation object it holds.
  const std::string module_json =
      "{\"schema\":\"vg.ir/v1\",\"version\":1,\"root_schema\":\"vg.test/pointer-graph\","
      "\"instructions\":["
      "{\"op\":\"load_ref\",\"allocation\":" +
      std::to_string(ref_id) + ",\"generation\":" + std::to_string(ref_generation) +
      ",\"representation_epoch\":0,\"offset\":0,\"size\":12},"
      "{\"op\":\"store_via\",\"allocation\":" +
      std::to_string(target_id) + ",\"generation\":" + std::to_string(target_generation) +
      ",\"representation_epoch\":0,\"offset\":0,\"size\":4,\"value\":42,\"ref_operand\":1}"
      "],"
      "\"effects\":[{\"allocation\":" +
      std::to_string(ref_id) +
      ",\"offset\":0,\"size\":12,\"access\":\"read\",\"representation_epoch\":0}],"
      "\"pointer_edges\":[{\"from_allocation\":" +
      std::to_string(ref_id) + ",\"field_offset\":0,\"to_allocation\":" + std::to_string(target_id) + "}]}";

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

  // The Task's full NodeRef is the sole route from the sealed TaskGraph to the
  // pointer-graph package and store_via execution; it is an executable
  // TaskGraph fact, not publication-only scaffolding.
  VgTaskGraphBuilderDesc builder_desc{};
  builder_desc.header.type = VG_STRUCTURE_TASK_GRAPH_BUILDER_DESC;
  builder_desc.header.size = sizeof(builder_desc);
  builder_desc.code_object = code_object;
  VgTaskGraphBuilder builder = nullptr;
  check(api.createTaskGraphBuilder(device, &builder_desc, &builder) == VG_SUCCESS, "createTaskGraphBuilder");

  VgTaskRecord task{};
  task.node = node_ref;
  task.root = ref_id;
  task.root_generation = ref_generation;
  task.shape = {1, 1, 1, 0};
  VgTaskId task_id{};
  check(api.taskGraphAppend(builder, &task, 1, &task_id) == VG_SUCCESS, "taskGraphAppend");

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

  VgSubmitDesc submit_desc{};
  submit_desc.header.type = VG_STRUCTURE_SUBMIT_DESC;
  submit_desc.header.size = sizeof(submit_desc);
  submit_desc.graph = graph;
  submit_desc.envelope = envelope;
  VgSubmission submission = nullptr;
  check(api.submit(device, &submit_desc, &submission) == VG_SUCCESS, "submit");

  const char* report_json = nullptr;
  check(api.getSubmissionLoweringReport(submission, &report_json) == VG_SUCCESS, "getSubmissionLoweringReport");
  if (check(report_json != nullptr, "lowering report json non-null")) {
    const std::string report(report_json);
    // canonical_json() serializes event object keys alphabetically (bytes,
    // classification, count, operation, reason), so this substring is
    // exactly the fragment Metal's per-Node package report produces,
    // LoweringClass::CachedObject, 1, ...) produces -- the same fact
    // run_pointer_graph() checks via compiled.report.events directly.
    const bool cached_object =
        report.find("\"classification\":1,\"count\":1,\"operation\":\"node_compute_package\"") != std::string::npos;
    check(cached_object, "per-Node package lowering classified CachedObject, matching run_pointer_graph()");
    std::fprintf(stderr, "e002-pointer-graph-via-abi: report json: %s\n", report.c_str());
  }

  const char* result_json = nullptr;
  check(api.getSubmissionExecutionResult(submission, &result_json) == VG_SUCCESS, "getSubmissionExecutionResult");
  if (check(result_json != nullptr, "execution result json non-null")) {
    const std::string result(result_json);
    // ExecutionResult::canonical_json() (core.cpp) serializes object keys
    // alphabetically, so a successful, non-poisoned execution's "ok" field
    // appears as this exact substring -- the same fact run_pointer_graph()
    // checks by reading core::Submission::result.ok directly.
    check(result.find("\"ok\":1") != std::string::npos,
          "execution result ok, matching run_pointer_graph()'s result.ok check");
    std::fprintf(stderr, "e002-pointer-graph-via-abi: execution result json: %s\n", result.c_str());
  }

  api.destroySubmission(submission);
  api.destroyExecutionEnvelope(envelope);
  api.destroyTaskGraph(graph);
  api.destroyTaskGraphBuilder(builder);
  api.destroyNode(node);
  api.destroyCodeObject(code_object);
  api.destroyArena(arena);
  api.destroyAddressDomain(domain);
  api.destroyDevice(device);
  api.closeAdapter(adapter);
  api.destroyRuntime(runtime);

  if (g_ok) {
    std::fprintf(stderr,
                 "e002-pointer-graph-via-abi: E002 matches run_pointer_graph() (golden path VG_SUCCESS, "
                 "node_compute_package CachedObject, execution result.ok). target-byte read-back is still not "
                 "reachable through vg.h and was not checked.\n");
  }
  return g_ok ? 0 : 1;
}
