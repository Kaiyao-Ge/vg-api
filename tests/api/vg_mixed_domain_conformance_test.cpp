// ADR-054 public-path acceptance: only the public header and vg_api are used.
// Internal publication identities/transition ownership remain covered by the
// assembler-driven MD-2/3/4 suites; the frozen ABI exposes neither structure.
#include "vg/vg.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s (%s)\n", message, vgGetLastDiagnostic());
    std::exit(1);
  }
}
template<class T> T desc(uint32_t type) {
  T value{};
  value.header.type = type;
  value.header.size = sizeof(value);
  return value;
}
struct Allocation { VgAllocation handle{}; uint64_t id{}; uint32_t generation{}; };
struct Program { VgCodeObject code{}; VgNode node{}; VgNodeRef ref{}; };

struct Fixture {
  VgApi& api;
  VgDevice device{};
  VgAddressDomain domain{};
  VgArena arena{};
  Allocation source, target, vertices;
  VgFacetRef sample{}, attachment{}, address{};
  std::vector<Program> programs;

  Fixture(VgApi& api_in, VgAdapter adapter) : api(api_in) {
    auto dd = desc<VgDeviceDesc>(VG_STRUCTURE_DEVICE_DESC);
    require(api.createDevice(adapter, &dd, &device) == VG_SUCCESS, "create device");
    auto ad = desc<VgAddressDomainDesc>(VG_STRUCTURE_ADDRESS_DOMAIN_DESC);
    ad.kind = VG_ADDRESS_DOMAIN_DEVICE_LOCAL;
    require(api.createAddressDomain(device, &ad, &domain) == VG_SUCCESS, "create domain");
    auto ar = desc<VgArenaDesc>(VG_STRUCTURE_ARENA_DESC);
    ar.domain = domain;
    require(api.createArena(device, &ar, &arena) == VG_SUCCESS, "create arena");
    source = allocate(64); target = allocate(64);
    const float quad[] = {
        -1,1,0,0,0, 1,1,0,1,0, -1,-1,0,0,1,
        1,1,0,1,0, 1,-1,0,1,1, -1,-1,0,0,1};
    vertices = allocate(sizeof(quad));
    require(api.writeAllocation(arena, vertices.handle, 0, quad, sizeof(quad)) == VG_SUCCESS,
            "upload vertices");
    sample = facet(source, 4, 4, VG_FACET_KIND_SAMPLE);
    attachment = facet(target, 4, 4, VG_FACET_KIND_ATTACHMENT);
    address = facet(vertices, sizeof(quad) / 4, 1, VG_FACET_KIND_ADDRESS);
  }
  ~Fixture() {
    for (auto& p : programs) { api.destroyNode(p.node); api.destroyCodeObject(p.code); }
    api.destroyArena(arena); api.destroyAddressDomain(domain); api.destroyDevice(device);
  }
  Allocation allocate(uint64_t bytes) {
    Allocation result;
    require(api.arenaAllocate(arena, bytes, &result.handle) == VG_SUCCESS, "allocate");
    require(api.getAllocationRef(result.handle, &result.id, &result.generation) == VG_SUCCESS,
            "allocation identity");
    return result;
  }
  VgFacetRef facet(Allocation a, uint32_t width, uint32_t height, uint32_t kind) {
    auto view = desc<VgCanonicalViewDesc>(VG_STRUCTURE_CANONICAL_VIEW_DESC);
    view.allocation = a.id; view.allocation_generation = a.generation;
    view.format = VG_PIXEL_FORMAT_RGBA8_UNORM; view.dimension = VG_VIEW_DIMENSION_TEXTURE_2D;
    view.width = width; view.height = height; view.array_layers = view.mip_levels = 1;
    view.swizzle_red = VG_SWIZZLE_RED; view.swizzle_green = VG_SWIZZLE_GREEN;
    view.swizzle_blue = VG_SWIZZLE_BLUE; view.swizzle_alpha = VG_SWIZZLE_ALPHA;
    VgFacetRef result{};
    require(api.acquireFacet(device, arena, &view, kind, &result) == VG_SUCCESS, "acquire facet");
    return result;
  }
  Program program(Allocation a, const char* op, uint64_t offset = 0, uint64_t size = 4,
                  int64_t value = 0, bool restricted = false) {
    const std::string access = std::strcmp(op, "load") == 0 ? "read" :
                               std::strcmp(op, "atomic_add") == 0 ? "atomic" : "write";
    const std::string identity = "\"allocation\":" + std::to_string(a.id) +
        ",\"offset\":" + std::to_string(offset) + ",\"size\":" + std::to_string(size);
    const std::string json = restricted ?
        "{\"root_schema\":\"vg.test.mixed/v1\",\"vertex_entry\":\"vertex_main\","
        "\"fragment_entry\":\"fragment_main\",\"vertex_abi\":\"vg.raster.vertex.xyzuv-packed/v1\","
        "\"source\":\"opaque restricted contract: rejected before pipeline compilation\"}" :
        "{\"schema\":\"vg.ir/v1\",\"version\":1,\"root_schema\":\"vg.test.mixed/v1\","
        "\"instructions\":[{\"op\":\"" + std::string(op) + "\"," + identity +
        ",\"generation\":" + std::to_string(a.generation) + ",\"value\":" +
        std::to_string(value) + "}],\"effects\":[{" + identity + ",\"access\":\"" +
        access + "\",\"representation_epoch\":0}]}";
    auto cd = desc<VgCodeObjectDesc>(VG_STRUCTURE_CODE_OBJECT_DESC);
    cd.bytes = json.data(); cd.byte_size = json.size();
    cd.format_tag = restricted ? "vg.msl.raster/v1" : "vg.ir/v1";
    Program p;
    require(api.loadCodeObject(device, &cd, &p.code) == VG_SUCCESS, "load distinct CodeObject");
    auto nd = desc<VgNodeDesc>(VG_STRUCTURE_NODE_DESC); nd.entry_name = "mixed_entry";
    require(api.createNode(p.code, &nd, &p.node) == VG_SUCCESS, "create Node");
    require(api.getNodeRef(p.node, &p.ref) == VG_SUCCESS, "get NodeRef");
    programs.push_back(p);
    return p;
  }
  VgTaskRecord compute(Program p, Allocation root) {
    VgTaskRecord t{}; t.node = p.ref; t.kind = VG_TASK_KIND_COMPUTE;
    t.root = root.id; t.root_generation = root.generation; t.shape = {1,1,1,0};
    return t;
  }
  VgTaskRecord raster(Program p) {
    VgTaskRecord t{}; t.node = p.ref; t.kind = VG_TASK_KIND_RASTER; t.shape = {1,1,1,0};
    t.root = source.id; t.root_generation = source.generation;
    t.topology = VG_TOPOLOGY_TRIANGLE_LIST; t.raster_facets = {sample, attachment};
    t.vertex_buffer_ref = address; t.raster_filter = VG_FILTER_NEAREST; t.raster_wrap = VG_WRAP_CLAMP;
    for (auto& channel : t.raster_tint) channel = 1;
    return t;
  }
  void fill(Allocation a, uint8_t value) {
    const std::array<uint8_t,64> bytes = [value] { std::array<uint8_t,64> b{}; b.fill(value); return b; }();
    require(api.writeAllocation(arena, a.handle, 0, bytes.data(), bytes.size()) == VG_SUCCESS, "fill pixels");
  }
  std::vector<uint8_t> read(Allocation a) {
    std::vector<uint8_t> result(64);
    require(api.readAllocation(arena, a.handle, 0, result.data(), result.size()) == VG_SUCCESS, "read pixels");
    return result;
  }
  std::string run(std::vector<VgTaskRecord> tasks, bool reverse_edge = false,
                  const char* unsupported = nullptr) {
    auto bd = desc<VgTaskGraphBuilderDesc>(VG_STRUCTURE_TASK_GRAPH_BUILDER_DESC);
    VgTaskGraphBuilder builder{};
    require(api.createTaskGraphBuilder(device, &bd, &builder) == VG_SUCCESS, "create graph builder");
    std::vector<VgTaskId> ids(tasks.size());
    require(api.taskGraphAppend(builder, tasks.data(), tasks.size(), ids.data()) == VG_SUCCESS, "append mixed tasks");
    if (tasks.size() == 2) {
      require(tasks[0].node.index != tasks[1].node.index ||
              tasks[0].node.generation != tasks[1].node.generation, "distinct complete NodeRefs");
      require(api.taskGraphAddDependency(builder, ids[reverse_edge ? 1 : 0], ids[reverse_edge ? 0 : 1]) == VG_SUCCESS,
              "cross-domain dependency");
    }
    auto sd = desc<VgSealDesc>(VG_STRUCTURE_SEAL_DESC);
    VgTaskGraph graph{};
    require(api.sealTaskGraph(builder, &sd, &graph) == VG_SUCCESS, "seal graph");
    auto ed = desc<VgExecutionEnvelopeDesc>(VG_STRUCTURE_EXECUTION_ENVELOPE_DESC);
    ed.arena = arena;
    std::vector<VgNodeRef> allowed;
    for (const auto& t : tasks) allowed.push_back(t.node);
    ed.allowed_nodes = allowed.data(); ed.allowed_node_count = allowed.size();
    VgExecutionEnvelope envelope{};
    require(api.createExecutionEnvelope(device, &ed, &envelope) == VG_SUCCESS, "authorize all Nodes");
    auto submit = desc<VgSubmitDesc>(VG_STRUCTURE_SUBMIT_DESC); submit.graph = graph; submit.envelope = envelope;
    const auto before_source = read(source), before_target = read(target);
    VgSubmission submission{};
    const auto result = api.submit(device, &submit, &submission);
    std::string report;
    if (unsupported) {
      require(result == VG_ERROR_UNSUPPORTED && submission == nullptr, "whole-plan Unsupported, no submission");
      require(std::strstr(vgGetLastDiagnostic(), unsupported) != nullptr, "exact unsupported contract diagnostic");
      require(read(source) == before_source && read(target) == before_target, "unsupported graph executes no supported subset");
    } else {
      require(result == VG_SUCCESS && submission != nullptr, "submit via public assembler/compile/commit path");
      const char* json{};
      require(api.getSubmissionExecutionResult(submission, &json) == VG_SUCCESS && json &&
              std::strstr(json, "\"ok\":1"), "successful execution result");
      require(api.getSubmissionLoweringReport(submission, &json) == VG_SUCCESS && json, "public lowering report");
      report = json;
      require(report.find("\"supported\":1") != std::string::npos, "supported lowering report");
      api.destroySubmission(submission);
    }
    api.destroyExecutionEnvelope(envelope); api.destroyTaskGraph(graph); api.destroyTaskGraphBuilder(builder);
    return report;
  }
};

void exercise(VgApi& api, VgAdapter adapter, uint32_t backend) {
  Fixture f(api, adapter);
  auto c = f.program(f.source, "store", 0, 4, 127);
  auto r = f.program(f.source, "load", 0, 4);
  if (backend == VG_BACKEND_VULKAN) {
    f.fill(f.source, 0); f.fill(f.target, 0);
    const auto report = f.run({f.compute(c, f.source), f.raster(r)});
    const auto source_after = f.read(f.source);
    const auto target_after = f.read(f.target);
    require(std::any_of(source_after.begin(), source_after.end(),
                        [](uint8_t value) { return value == 127; }),
            "Vulkan compute write is visible before Raster sampling");
    require(std::any_of(target_after.begin(), target_after.end(),
                        [](uint8_t value) { return value != 0; }),
            "Vulkan Raster produced target pixels");
    require(report.find("\"operation\":\"vulkan_raster_draw\"") != std::string::npos,
            "Vulkan Stage7 reports the Raster draw");
    require(report.find("\"operation\":\"task_publication_dispatch\"") != std::string::npos,
            "Vulkan Stage7 publishes the sealed compute task");
    require(report.find("\"operation\":\"raster_task_publication\"") != std::string::npos,
            "Vulkan Stage7 publishes the sealed Raster task");
    return;
  }
  // Reverse storage order proves that public TaskGraph dependencies, not
  // append order or a first/global Node projection, determine execution.
  f.run({f.raster(r), f.compute(c, f.source)}, true);
  const auto first = f.read(f.target);
  require(std::any_of(first.begin(), first.end(), [](uint8_t v) { return v == 127; }),
          "Compute stores become visible to Raster sampling/resolve");
  f.fill(f.source, 0); f.fill(f.target, 0);
  f.run({f.raster(r), f.compute(c, f.source)}, true);
  require(f.read(f.target) == first, "successive public submissions preserve mixed results and release holds");

  // Derive a covered pixel from a raster-only oracle, then atomically modify
  // those bytes after Raster. A store constant alone could not prove R->C.
  f.fill(f.source, 127); f.fill(f.target, 0);
  f.run({f.raster(r)});
  const auto oracle = f.read(f.target);
  uint64_t offset = UINT64_MAX, baseline = 0;
  for (size_t i = 0; i + sizeof(baseline) <= oracle.size(); i += sizeof(baseline)) {
    std::memcpy(&baseline, oracle.data() + i, sizeof(baseline));
    if (baseline != 0) { offset = i; break; }
  }
  require(offset != UINT64_MAX, "raster oracle has an observable covered pixel");
  auto atomic = f.program(f.target, "atomic_add", offset, 8, 1);
  f.fill(f.target, 0);
  const auto report = f.run({f.compute(atomic, f.target), f.raster(r)}, true);
  auto expected = oracle;
  ++baseline; std::memcpy(expected.data() + offset, &baseline, sizeof(baseline));
  require(f.read(f.target) == expected, "Raster output is consumed by the distinct Compute Node");
  if (backend == VG_BACKEND_METAL) {
    // The existing diagnostic JSON encodes HostAssisted as classification 3;
    // scope the assertion to metal_pipeline, not unrelated host publication.
    const auto pipeline = report.find("\"operation\":\"metal_pipeline\"");
    require(pipeline != std::string::npos, "R->C compute pipeline report exists");
    const auto start = report.rfind('{', pipeline), end = report.find('}', pipeline);
    require(report.substr(start, end - start).find("\"classification\":3") != std::string::npos,
            "64-bit R->C atomic reports HostAssisted, not native fence evidence");
  }
  // ADR-054 retains restricted mixed narrowing on Reference too. Distinct
  // NodeRefs must reach backend Unsupported, not a same-Node domain error.
  auto restricted = f.program(f.source, "load", 0, 4, 0, true);
  f.fill(f.source, 0); f.fill(f.target, 0);
  f.run({f.compute(c, f.source), f.raster(restricted)}, false, "restricted user raster");
  if (backend == VG_BACKEND_REFERENCE) {
    require(std::strstr(vgGetLastDiagnostic(),
        ("NodeRef{" + std::to_string(restricted.ref.index) + "," +
         std::to_string(restricted.ref.generation) + "} domain=Raster").c_str()) != nullptr,
        "Reference restricted mixed diagnostic identifies the Raster Node");
    f.fill(f.source, 127);
    const auto restricted_report = f.run({f.raster(restricted)});
    require(f.read(f.target) == oracle, "Reference restricted raster-only oracle remains supported");
    require(restricted_report.find("raster_user_shader") != std::string::npos,
            "Reference still discloses its restricted raster-only shading contract");
  }
}
}  // namespace

int main(int argc, char** argv) {
  const std::string name = argc > 1 ? argv[1] : "reference";
  const uint32_t backend = name == "metal" ? VG_BACKEND_METAL :
                           name == "vulkan" ? VG_BACKEND_VULKAN : VG_BACKEND_REFERENCE;
  VgApi api{}; api.size = sizeof(api);
  require(vgGetApi(VG_API_VERSION_1_7, &api) == VG_SUCCESS, "API v1.7");
  auto rd = desc<VgRuntimeDesc>(VG_STRUCTURE_RUNTIME_DESC); VgRuntime runtime{};
  require(api.createRuntime(&rd, &runtime) == VG_SUCCESS, "create runtime");
  uint32_t count{};
  require(api.enumerateAdapters(runtime, &count, nullptr) == VG_SUCCESS, "count adapters");
  std::vector<VgAdapterInfo> adapters(count);
  for (auto& a : adapters) a = desc<VgAdapterInfo>(VG_STRUCTURE_ADAPTER_INFO);
  require(api.enumerateAdapters(runtime, &count, adapters.data()) == VG_SUCCESS, "enumerate adapters");
  auto selected = std::find_if(adapters.begin(), adapters.end(), [backend](const auto& a) { return a.backend_kind == backend; });
  if (selected == adapters.end()) { api.destroyRuntime(runtime); return 77; }
  VgAdapter adapter{};
  require(api.openAdapter(runtime, selected->stable_uuid, &adapter) == VG_SUCCESS, "open selected adapter");
  exercise(api, adapter, backend);
  api.closeAdapter(adapter); api.destroyRuntime(runtime);
  return 0;
}
