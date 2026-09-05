#include "vg/vg.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
bool ok = true;
void check(bool v, const char *s) {
  if (!v) {
    std::fprintf(stderr, "FAIL %s: %s\n", s, vgGetLastDiagnostic());
    ok = false;
  }
}
template <class T> T desc(uint32_t type) {
  T v{};
  v.header.type = type;
  v.header.size = sizeof(v);
  return v;
}
} // namespace

int main() {
  VgApi api{};
  api.size = sizeof(api);
  if (vgGetApi(VG_API_VERSION_1_6, &api) != VG_SUCCESS) {
    std::fprintf(stderr, "FAIL get api: %s\n", vgGetLastDiagnostic());
    return 1;
  }
  auto rd = desc<VgRuntimeDesc>(VG_STRUCTURE_RUNTIME_DESC);
  VgRuntime runtime{};
  check(api.createRuntime(&rd, &runtime) == VG_SUCCESS, "runtime");
  uint32_t n = 0;
  check(api.enumerateAdapters(runtime, &n, nullptr) == VG_SUCCESS,
        "adapter count");
  std::vector<VgAdapterInfo> adapters(n);
  for (auto &a : adapters)
    a = desc<VgAdapterInfo>(VG_STRUCTURE_ADAPTER_INFO);
  uint32_t written = n;
  check(api.enumerateAdapters(runtime, &written, adapters.data()) == VG_SUCCESS,
        "adapter list");
  const VgAdapterInfo *selected = nullptr;
  for (const auto &a : adapters)
    if (a.backend_kind == VG_BACKEND_VULKAN)
      selected = &a;
  if (!selected) {
    std::fprintf(stderr, "Vulkan adapter required; no fallback is permitted\n");
    api.destroyRuntime(runtime);
    return 1;
  }
  VgAdapter adapter{};
  check(api.openAdapter(runtime, selected->stable_uuid, &adapter) == VG_SUCCESS,
        "open Vulkan");
  auto dd = desc<VgDeviceDesc>(VG_STRUCTURE_DEVICE_DESC);
  VgDevice device{};
  check(api.createDevice(adapter, &dd, &device) == VG_SUCCESS, "device");
  auto ad = desc<VgAddressDomainDesc>(VG_STRUCTURE_ADDRESS_DOMAIN_DESC);
  ad.kind = VG_ADDRESS_DOMAIN_DEVICE_LOCAL;
  VgAddressDomain domain{};
  check(api.createAddressDomain(device, &ad, &domain) == VG_SUCCESS, "domain");
  auto ard = desc<VgArenaDesc>(VG_STRUCTURE_ARENA_DESC);
  ard.domain = domain;
  VgArena arena{};
  check(api.createArena(device, &ard, &arena) == VG_SUCCESS, "arena");
  VgAllocation holder{}, target{};
  check(api.arenaAllocate(arena, 16, &holder) == VG_SUCCESS, "holder");
  check(api.arenaAllocate(arena, 4, &target) == VG_SUCCESS, "target");
  uint64_t hid{}, tid{};
  uint32_t hg{}, tg{};
  check(api.getAllocationRef(holder, &hid, &hg) == VG_SUCCESS, "holder ref");
  check(api.getAllocationRef(target, &tid, &tg) == VG_SUCCESS, "target ref");
  unsigned char wire[12]{};
  std::memcpy(wire, &tid, 8);
  std::memcpy(wire + 8, &tg, 4);
  check(api.writeAllocation(arena, holder, 0, wire, 12) == VG_SUCCESS,
        "pointer bytes");
  const std::string json =
      "{\"schema\":\"vg.ir/v1\",\"version\":1,\"root_schema\":\"vg.test/"
      "vulkan-pointer\",\"instructions\":[{\"op\":\"load_ref\","
      "\"allocation\":" +
      std::to_string(hid) + ",\"generation\":" + std::to_string(hg) +
      ",\"representation_epoch\":0,\"offset\":0,\"size\":12},{\"op\":\"store_"
      "via\",\"allocation\":" +
      std::to_string(tid) + ",\"generation\":" + std::to_string(tg) +
      ",\"representation_epoch\":0,\"offset\":0,\"size\":4,\"value\":42,\"ref_"
      "operand\":1}],\"effects\":[{\"allocation\":" +
      std::to_string(hid) +
      ",\"offset\":0,\"size\":12,\"access\":\"read\",\"representation_epoch\":"
      "0}],\"pointer_edges\":[{\"from_allocation\":" +
      std::to_string(hid) +
      ",\"field_offset\":0,\"to_allocation\":" + std::to_string(tid) + "}]}";
  auto cd = desc<VgCodeObjectDesc>(VG_STRUCTURE_CODE_OBJECT_DESC);
  cd.bytes = json.data();
  cd.byte_size = json.size();
  cd.format_tag = "vg.ir/v1";
  VgCodeObject co{};
  check(api.loadCodeObject(device, &cd, &co) == VG_SUCCESS, "code");
  auto nd = desc<VgNodeDesc>(VG_STRUCTURE_NODE_DESC);
  nd.entry_name = "main";
  VgNode node{};
  VgNodeRef ref{};
  check(api.createNode(co, &nd, &node) == VG_SUCCESS, "node");
  check(api.getNodeRef(node, &ref) == VG_SUCCESS, "node ref");
  auto bd = desc<VgTaskGraphBuilderDesc>(VG_STRUCTURE_TASK_GRAPH_BUILDER_DESC);
  VgTaskGraphBuilder b{};
  check(api.createTaskGraphBuilder(device, &bd, &b) == VG_SUCCESS, "builder");
  VgTaskRecord task{};
  task.node = ref;
  task.root = hid;
  task.root_generation = hg;
  task.kind = VG_TASK_KIND_COMPUTE;
  task.shape = {1, 1, 1, 0};
  VgTaskId id{};
  check(api.taskGraphAppend(b, &task, 1, &id) == VG_SUCCESS, "append");
  auto sd = desc<VgSealDesc>(VG_STRUCTURE_SEAL_DESC);
  VgTaskGraph graph{};
  check(api.sealTaskGraph(b, &sd, &graph) == VG_SUCCESS, "seal");
  auto ed = desc<VgExecutionEnvelopeDesc>(VG_STRUCTURE_EXECUTION_ENVELOPE_DESC);
  ed.arena = arena;
  VgExecutionEnvelope envelope{};
  check(api.createExecutionEnvelope(device, &ed, &envelope) == VG_SUCCESS,
        "envelope");
  auto subd = desc<VgSubmitDesc>(VG_STRUCTURE_SUBMIT_DESC);
  subd.graph = graph;
  subd.envelope = envelope;
  VgSubmission sub{};
  check(api.submit(device, &subd, &sub) == VG_SUCCESS, "Vulkan submit");
  const char *report{};
  check(api.getSubmissionLoweringReport(sub, &report) == VG_SUCCESS && report &&
            std::strstr(report, "\"classification\":1,\"count\":1,"
                                "\"operation\":\"node_compute_package\""),
        "CachedObject node package report");
  const char *execution{};
  check(api.getSubmissionExecutionResult(sub, &execution) == VG_SUCCESS &&
            execution && std::strstr(execution, "\"ok\":1") &&
            std::strstr(execution, "\"outputs_valid\":1"),
        "successful valid execution result");
  unsigned char result[4]{};
  check(api.readAllocation(arena, target, 0, result, 4) == VG_SUCCESS &&
            result[0] == 42 && result[1] == 42 && result[2] == 42 &&
            result[3] == 42,
        "observable four-byte store");
  if (sub)
    api.destroySubmission(sub);
  api.destroyExecutionEnvelope(envelope);
  api.destroyTaskGraph(graph);
  api.destroyTaskGraphBuilder(b);
  api.destroyNode(node);
  api.destroyCodeObject(co);
  api.destroyArena(arena);
  api.destroyAddressDomain(domain);
  api.destroyDevice(device);
  api.closeAdapter(adapter);
  api.destroyRuntime(runtime);
  return ok ? 0 : 1;
}
