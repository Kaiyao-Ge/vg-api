// Conformance coverage for the device-scoped Node capability namespace.
//
// This intentionally uses only <vg/vg.h> and the reference adapter.  It is
// an acceptance test for the public semantic path, not a unit test coupled to
// NodeTable or ExecutionPlan internals.  In particular, the two CodeObjects
// use distinct effects so an implementation that merely stores two refs but
// still executes one graph-wide module cannot pass.
#include "vg/vg.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool ok = true;

bool check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s (diagnostic: %s)\n", what, vgGetLastDiagnostic());
    ok = false;
  }
  return condition;
}

template <typename T>
T init(uint32_t type) {
  T value{};
  value.header.type = type;
  value.header.size = sizeof(T);
  return value;
}

const VgAdapterInfo* pick_reference(const std::vector<VgAdapterInfo>& adapters) {
  for (const auto& adapter : adapters) {
    if (adapter.backend_kind == VG_BACKEND_REFERENCE) return &adapter;
  }
  return nullptr;
}

std::string store_module(uint64_t allocation, uint32_t generation, uint8_t value) {
  return "{\"schema\":\"vg.ir/v1\",\"version\":1,\"root_schema\":\"vg.test.multicode/v1\","
         "\"instructions\":[{\"op\":\"store\",\"allocation\":" + std::to_string(allocation) +
         ",\"generation\":" + std::to_string(generation) + ",\"offset\":0,\"size\":4,\"value\":" +
         std::to_string(value) + "}],\"effects\":[{\"allocation\":" + std::to_string(allocation) +
         ",\"offset\":0,\"size\":4,\"access\":\"write\",\"representation_epoch\":0}]}";
}

std::string load_then_store_module(uint64_t input, uint32_t input_generation, uint64_t output,
                                   uint32_t output_generation, uint8_t value) {
  return "{\"schema\":\"vg.ir/v1\",\"version\":1,\"root_schema\":\"vg.test.multicode/v1\","
         "\"instructions\":[{\"op\":\"load\",\"allocation\":" + std::to_string(input) +
         ",\"generation\":" + std::to_string(input_generation) +
         ",\"offset\":0,\"size\":4},{\"op\":\"store\",\"allocation\":" + std::to_string(output) +
         ",\"generation\":" + std::to_string(output_generation) + ",\"offset\":0,\"size\":4,\"value\":" +
         std::to_string(value) + "}],\"effects\":[{\"allocation\":" + std::to_string(input) +
         ",\"offset\":0,\"size\":4,\"access\":\"read\",\"representation_epoch\":0},{\"allocation\":" +
         std::to_string(output) + ",\"offset\":0,\"size\":4,\"access\":\"write\",\"representation_epoch\":0}]}";
}

std::string load_then_overwrite_module(uint64_t allocation, uint32_t generation, uint8_t value) {
  return "{\"schema\":\"vg.ir/v1\",\"version\":1,\"root_schema\":\"vg.test.multicode/v1\","
         "\"instructions\":[{\"op\":\"load\",\"allocation\":" + std::to_string(allocation) +
         ",\"generation\":" + std::to_string(generation) +
         ",\"offset\":0,\"size\":4},{\"op\":\"store\",\"allocation\":" +
         std::to_string(allocation) + ",\"generation\":" + std::to_string(generation) +
         ",\"offset\":0,\"size\":4,\"value\":" + std::to_string(value) +
         "}],\"effects\":[{\"allocation\":" + std::to_string(allocation) +
         ",\"offset\":0,\"size\":4,\"access\":\"read\",\"representation_epoch\":0},"
         "{\"allocation\":" + std::to_string(allocation) +
         ",\"offset\":0,\"size\":4,\"access\":\"write\",\"representation_epoch\":0}]}";
}

std::string atomic_add_module(uint64_t allocation, uint32_t generation, int64_t value) {
  return "{\"schema\":\"vg.ir/v1\",\"version\":1,\"root_schema\":\"vg.test.multicode/v1\","
         "\"instructions\":[{\"op\":\"atomic_add\",\"allocation\":" + std::to_string(allocation) +
         ",\"generation\":" + std::to_string(generation) + ",\"offset\":0,\"size\":8,\"value\":" +
         std::to_string(value) + "}],\"effects\":[{\"allocation\":" + std::to_string(allocation) +
         ",\"offset\":0,\"size\":8,\"access\":\"atomic\",\"representation_epoch\":0}]}";
}

VgCodeObject load(VgApi& api, VgDevice device, const std::string& module, const char* label) {
  VgCodeObjectDesc desc = init<VgCodeObjectDesc>(VG_STRUCTURE_CODE_OBJECT_DESC);
  desc.bytes = module.data();
  desc.byte_size = module.size();
  desc.format_tag = "vg.ir/v1";
  VgCodeObject code = nullptr;
  check(api.loadCodeObject(device, &desc, &code) == VG_SUCCESS, label);
  return code;
}

VgNode create_node(VgApi& api, VgCodeObject code, const char* entry, VgNodeRef* ref, const char* label) {
  VgNodeDesc desc = init<VgNodeDesc>(VG_STRUCTURE_NODE_DESC);
  desc.entry_name = entry;
  VgNode node = nullptr;
  if (!check(api.createNode(code, &desc, &node) == VG_SUCCESS, label)) return nullptr;
  check(api.getNodeRef(node, ref) == VG_SUCCESS, "getNodeRef");
  return node;
}

}  // namespace

int main() {
  VgApi api{};
  api.size = sizeof(api);
  if (!check(vgGetApi(VG_API_VERSION_1_7, &api) == VG_SUCCESS, "vgGetApi v1.7")) return 1;

  VgRuntimeDesc runtime_desc = init<VgRuntimeDesc>(VG_STRUCTURE_RUNTIME_DESC);
  VgRuntime runtime = nullptr;
  if (!check(api.createRuntime(&runtime_desc, &runtime) == VG_SUCCESS, "createRuntime")) return 1;
  uint32_t adapter_count = 0;
  check(api.enumerateAdapters(runtime, &adapter_count, nullptr) == VG_SUCCESS, "enumerateAdapters count");
  std::vector<VgAdapterInfo> adapters(adapter_count);
  for (auto& adapter : adapters) adapter = init<VgAdapterInfo>(VG_STRUCTURE_ADAPTER_INFO);
  uint32_t written = adapter_count;
  check(api.enumerateAdapters(runtime, &written, adapters.data()) == VG_SUCCESS, "enumerateAdapters fill");
  const VgAdapterInfo* info = pick_reference(adapters);
  if (!check(info != nullptr, "reference adapter is available")) return 1;

  VgAdapter adapter = nullptr;
  VgDevice device = nullptr;
  VgAddressDomain domain = nullptr;
  VgArena arena = nullptr;
  check(api.openAdapter(runtime, info->stable_uuid, &adapter) == VG_SUCCESS, "open reference adapter");
  VgDeviceDesc device_desc = init<VgDeviceDesc>(VG_STRUCTURE_DEVICE_DESC);
  check(api.createDevice(adapter, &device_desc, &device) == VG_SUCCESS, "create primary device");
  VgAddressDomainDesc domain_desc = init<VgAddressDomainDesc>(VG_STRUCTURE_ADDRESS_DOMAIN_DESC);
  domain_desc.kind = VG_ADDRESS_DOMAIN_DEVICE_LOCAL;
  check(api.createAddressDomain(device, &domain_desc, &domain) == VG_SUCCESS, "create address domain");
  VgArenaDesc arena_desc = init<VgArenaDesc>(VG_STRUCTURE_ARENA_DESC);
  arena_desc.domain = domain;
  check(api.createArena(device, &arena_desc, &arena) == VG_SUCCESS, "create arena");

  VgAllocation intermediate = nullptr;
  VgAllocation output = nullptr;
  check(api.arenaAllocate(arena, 4, &intermediate) == VG_SUCCESS, "allocate intermediate");
  check(api.arenaAllocate(arena, 4, &output) == VG_SUCCESS, "allocate output");
  uint64_t intermediate_id = 0, output_id = 0;
  uint32_t intermediate_generation = 0, output_generation = 0;
  check(api.getAllocationRef(intermediate, &intermediate_id, &intermediate_generation) == VG_SUCCESS,
        "get intermediate ref");
  check(api.getAllocationRef(output, &output_id, &output_generation) == VG_SUCCESS, "get output ref");

  const std::string producer = store_module(intermediate_id, intermediate_generation, 0x2a);
  const std::string consumer = load_then_store_module(intermediate_id, intermediate_generation, output_id,
                                                       output_generation, 0xb2);
  VgCodeObject code_a = load(api, device, producer, "load producer CodeObject");
  VgCodeObject code_b = load(api, device, consumer, "load consumer CodeObject");
  VgNodeRef ref_a{}, ref_b{};
  VgNode node_a = create_node(api, code_a, "producer", &ref_a, "create producer Node");
  VgNode node_b = create_node(api, code_b, "consumer", &ref_b, "create consumer Node");
  check(ref_a.index != ref_b.index || ref_a.generation != ref_b.generation,
        "Nodes from distinct CodeObjects have distinct device-scoped NodeRefs");

  // Keep the legacy non-null compatibility hint: it must not constrain the
  // Node set accepted by the builder.
  VgTaskGraphBuilderDesc builder_desc = init<VgTaskGraphBuilderDesc>(VG_STRUCTURE_TASK_GRAPH_BUILDER_DESC);
  builder_desc.code_object = code_a;
  VgTaskGraphBuilder builder = nullptr;
  check(api.createTaskGraphBuilder(device, &builder_desc, &builder) == VG_SUCCESS,
        "create device-scoped graph builder");
  VgTaskRecord tasks[2]{};
  tasks[0].node = ref_a;
  tasks[0].root = intermediate_id;
  tasks[0].root_generation = intermediate_generation;
  tasks[0].shape = {1, 1, 1, 0};
  tasks[1].node = ref_b;
  tasks[1].root = output_id;
  tasks[1].root_generation = output_generation;
  tasks[1].shape = {1, 1, 1, 0};
  VgTaskId task_ids[2]{};
  check(api.taskGraphAppend(builder, tasks, 2, task_ids) == VG_SUCCESS,
        "append Nodes from two CodeObjects to one graph");
  check(api.taskGraphAddDependency(builder, task_ids[0], task_ids[1]) == VG_SUCCESS,
        "producer precedes consumer");
  VgSealDesc seal_desc = init<VgSealDesc>(VG_STRUCTURE_SEAL_DESC);
  VgTaskGraph graph = nullptr;
  check(api.sealTaskGraph(builder, &seal_desc, &graph) == VG_SUCCESS, "seal multi-CodeObject graph");

  // The full identity must be authenticated: an index match alone must not
  // authorize a Node after its generation changes.
  VgNodeRef wrong_generation{ref_a.index, ref_a.generation + 1};
  VgExecutionEnvelopeDesc bad_envelope_desc = init<VgExecutionEnvelopeDesc>(VG_STRUCTURE_EXECUTION_ENVELOPE_DESC);
  bad_envelope_desc.arena = arena;
  bad_envelope_desc.allowed_nodes = &wrong_generation;
  bad_envelope_desc.allowed_node_count = 1;
  VgExecutionEnvelope bad_envelope = nullptr;
  check(api.createExecutionEnvelope(device, &bad_envelope_desc, &bad_envelope) == VG_ERROR_INVALID_ARGUMENT,
        "Envelope rejects a NodeRef with the right index and wrong generation");

  // Authorization is checked by the core ExecutionPlanAssembler at submit,
  // not by an API-side scan.  A live, generation-correct subset remains an
  // invalid envelope for this two-Node graph.
  VgExecutionEnvelopeDesc subset_envelope_desc = init<VgExecutionEnvelopeDesc>(VG_STRUCTURE_EXECUTION_ENVELOPE_DESC);
  subset_envelope_desc.arena = arena;
  subset_envelope_desc.allowed_nodes = &ref_a;
  subset_envelope_desc.allowed_node_count = 1;
  VgExecutionEnvelope subset_envelope = nullptr;
  check(api.createExecutionEnvelope(device, &subset_envelope_desc, &subset_envelope) == VG_SUCCESS,
        "create subset envelope");
  VgSubmitDesc subset_submit = init<VgSubmitDesc>(VG_STRUCTURE_SUBMIT_DESC);
  subset_submit.graph = graph;
  subset_submit.envelope = subset_envelope;
  VgSubmission subset_submission = nullptr;
  check(api.submit(device, &subset_submit, &subset_submission) == VG_ERROR_INVALID_ARGUMENT,
        "assembler rejects an envelope that omits a graph Node");
  api.destroyExecutionEnvelope(subset_envelope);

  VgNodeRef allowed[] = {ref_a, ref_b};
  VgExecutionEnvelopeDesc envelope_desc = init<VgExecutionEnvelopeDesc>(VG_STRUCTURE_EXECUTION_ENVELOPE_DESC);
  envelope_desc.arena = arena;
  envelope_desc.allowed_nodes = allowed;
  envelope_desc.allowed_node_count = 2;
  VgAccessCertificateDesc certificate_desc = init<VgAccessCertificateDesc>(VG_STRUCTURE_ACCESS_CERTIFICATE_DESC);
  certificate_desc.mode = VG_ACCESS_CERTIFICATE_MODE_CERTIFIED_PINNED;
  // CertifiedPinned is caller authority.  Both the producer's intermediate
  // and the consumer's output must be declared; assembly must not grow this
  // set from inferred effects.
  const VgAccessRange certificate_ranges[] = {
      {intermediate, 0, 4, 0, 0, 0}, {output, 0, 4, 0, 0, 0}};
  certificate_desc.range_count = 2;
  certificate_desc.ranges = certificate_ranges;
  envelope_desc.access_certificate = &certificate_desc;
  VgExecutionEnvelope envelope = nullptr;
  check(api.createExecutionEnvelope(device, &envelope_desc, &envelope) == VG_SUCCESS,
        "create full-identity multi-Node envelope");

  // Node B must retain an immutable CodeObject snapshot.  Destroying the
  // host handle here must not turn a valid graph into a UAF or silently make
  // task B run code A.
  api.destroyCodeObject(code_b);
  VgSubmitDesc submit_desc = init<VgSubmitDesc>(VG_STRUCTURE_SUBMIT_DESC);
  submit_desc.graph = graph;
  submit_desc.envelope = envelope;
  VgSubmission first = nullptr;
  check(api.submit(device, &submit_desc, &first) == VG_SUCCESS, "submit multi-CodeObject graph");
  uint8_t bytes[4]{};
  check(api.readAllocation(arena, intermediate, 0, bytes, sizeof(bytes)) == VG_SUCCESS &&
            bytes[0] == 0x2a && bytes[1] == 0x2a && bytes[2] == 0x2a && bytes[3] == 0x2a,
        "producer Node executed its CodeObject");
  check(api.readAllocation(arena, output, 0, bytes, sizeof(bytes)) == VG_SUCCESS &&
            bytes[0] == 0xb2 && bytes[1] == 0xb2 && bytes[2] == 0xb2 && bytes[3] == 0xb2,
        "consumer Node read producer input and executed its own CodeObject");
  const char* first_result = nullptr;
  check(api.getSubmissionExecutionResult(first, &first_result) == VG_SUCCESS && first_result != nullptr &&
            std::strstr(first_result, "\"ok\":1") != nullptr,
        "multi-CodeObject execution result is successful");
  const char* first_report = nullptr;
  check(api.getSubmissionLoweringReport(first, &first_report) == VG_SUCCESS && first_report != nullptr &&
            std::strstr(first_report, "\"count\":2,\"operation\":\"access_certificate\"") != nullptr,
        "certificate includes the consumer CodeObject's exclusive output allocation");
  api.destroySubmission(first);

  // A sealed graph is immutable but reusable.  A second submit must execute
  // both Node packages again rather than retaining one consumed global module.
  const uint8_t zeroes[4]{};
  check(api.writeAllocation(arena, intermediate, 0, zeroes, sizeof(zeroes)) == VG_SUCCESS,
        "clear intermediate before repeat submit");
  check(api.writeAllocation(arena, output, 0, zeroes, sizeof(zeroes)) == VG_SUCCESS,
        "clear output before repeat submit");
  VgSubmission second = nullptr;
  check(api.submit(device, &submit_desc, &second) == VG_SUCCESS, "repeat submit multi-CodeObject graph");
  check(api.readAllocation(arena, output, 0, bytes, sizeof(bytes)) == VG_SUCCESS && bytes[0] == 0xb2,
        "repeat submit re-executed consumer Node");
  api.destroySubmission(second);

  // Store order and dependency order must not be conflated.  The consumer is
  // appended first, but depends on the producer appended second.  Executing
  // in storage order would overwrite the producer's value with the wrong
  // consumer value; dependency/topological order produces the intended
  // consumer result, a deliberately distinct observable outcome.
  VgAllocation ordered_value = nullptr;
  check(api.arenaAllocate(arena, 4, &ordered_value) == VG_SUCCESS, "allocate reverse-order value");
  uint64_t ordered_id = 0;
  uint32_t ordered_generation = 0;
  check(api.getAllocationRef(ordered_value, &ordered_id, &ordered_generation) == VG_SUCCESS,
        "get reverse-order value ref");
  const std::string reverse_producer = store_module(ordered_id, ordered_generation, 0x2a);
  const std::string reverse_consumer = load_then_overwrite_module(ordered_id, ordered_generation, 0xb2);
  VgCodeObject reverse_producer_code = load(api, device, reverse_producer, "load reverse-order producer");
  VgCodeObject reverse_consumer_code = load(api, device, reverse_consumer, "load reverse-order consumer");
  VgNodeRef reverse_producer_ref{}, reverse_consumer_ref{};
  VgNode reverse_producer_node = create_node(api, reverse_producer_code, "reverse-producer",
                                              &reverse_producer_ref, "create reverse-order producer Node");
  VgNode reverse_consumer_node = create_node(api, reverse_consumer_code, "reverse-consumer",
                                              &reverse_consumer_ref, "create reverse-order consumer Node");
  VgTaskGraphBuilder reverse_builder = nullptr;
  check(api.createTaskGraphBuilder(device, &builder_desc, &reverse_builder) == VG_SUCCESS,
        "create reverse-order graph builder");
  VgTaskRecord reverse_tasks[2]{};
  reverse_tasks[0].node = reverse_consumer_ref;  // Stored first, must run second.
  reverse_tasks[0].root = ordered_id;
  reverse_tasks[0].root_generation = ordered_generation;
  reverse_tasks[0].shape = {1, 1, 1, 0};
  reverse_tasks[1].node = reverse_producer_ref;  // Stored second, must run first.
  reverse_tasks[1].root = ordered_id;
  reverse_tasks[1].root_generation = ordered_generation;
  reverse_tasks[1].shape = {1, 1, 1, 0};
  VgTaskId reverse_ids[2]{};
  check(api.taskGraphAppend(reverse_builder, reverse_tasks, 2, reverse_ids) == VG_SUCCESS,
        "append reverse-storage-order tasks");
  check(api.taskGraphAddDependency(reverse_builder, reverse_ids[1], reverse_ids[0]) == VG_SUCCESS,
        "reverse-storage-order dependency producer-to-consumer");
  VgTaskGraph reverse_graph = nullptr;
  check(api.sealTaskGraph(reverse_builder, &seal_desc, &reverse_graph) == VG_SUCCESS,
        "seal reverse-order graph");
  VgNodeRef reverse_allowed[] = {reverse_producer_ref, reverse_consumer_ref};
  VgExecutionEnvelopeDesc reverse_envelope_desc = init<VgExecutionEnvelopeDesc>(VG_STRUCTURE_EXECUTION_ENVELOPE_DESC);
  reverse_envelope_desc.arena = arena;
  reverse_envelope_desc.allowed_nodes = reverse_allowed;
  reverse_envelope_desc.allowed_node_count = 2;
  VgExecutionEnvelope reverse_envelope = nullptr;
  check(api.createExecutionEnvelope(device, &reverse_envelope_desc, &reverse_envelope) == VG_SUCCESS,
        "create reverse-order envelope");
  VgSubmitDesc reverse_submit = submit_desc;
  reverse_submit.graph = reverse_graph;
  reverse_submit.envelope = reverse_envelope;
  VgSubmission reverse_submission = nullptr;
  check(api.submit(device, &reverse_submit, &reverse_submission) == VG_SUCCESS,
        "submit reverse-storage-order graph");
  uint8_t ordered_result[4]{};
  check(api.readAllocation(arena, ordered_value, 0, ordered_result, sizeof(ordered_result)) == VG_SUCCESS &&
            ordered_result[0] == 0xb2 && ordered_result[1] == 0xb2 &&
            ordered_result[2] == 0xb2 && ordered_result[3] == 0xb2,
        "execution follows dependency topology rather than Task storage order");
  api.destroySubmission(reverse_submission);
  api.destroyExecutionEnvelope(reverse_envelope);
  api.destroyTaskGraph(reverse_graph);
  api.destroyTaskGraphBuilder(reverse_builder);
  api.destroyNode(reverse_consumer_node);
  api.destroyNode(reverse_producer_node);
  api.destroyCodeObject(reverse_consumer_code);
  api.destroyCodeObject(reverse_producer_code);

  // Package deduplication must not deduplicate Task execution.  Both records
  // name one Node but carry distinct task metadata; each invocation must run
  // and contribute one atomic increment.  The canonical-compute interpreter
  // currently has no public observation of root/shape/payload consumption or
  // package-cache hits, so this asserts the semantic part (per-Task work)
  // while keeping those independently encoded fields distinct for later
  // Node-contract coverage.
  VgAllocation same_node_value = nullptr;
  VgAllocation root_one = nullptr;
  VgAllocation root_two = nullptr;
  check(api.arenaAllocate(arena, 8, &same_node_value) == VG_SUCCESS, "allocate same-Node accumulator");
  check(api.arenaAllocate(arena, 8, &root_one) == VG_SUCCESS, "allocate first same-Node root");
  check(api.arenaAllocate(arena, 8, &root_two) == VG_SUCCESS, "allocate second same-Node root");
  uint64_t same_node_id = 0, root_one_id = 0, root_two_id = 0;
  uint32_t same_node_generation = 0, root_one_generation = 0, root_two_generation = 0;
  check(api.getAllocationRef(same_node_value, &same_node_id, &same_node_generation) == VG_SUCCESS,
        "get same-Node accumulator ref");
  check(api.getAllocationRef(root_one, &root_one_id, &root_one_generation) == VG_SUCCESS,
        "get first same-Node root ref");
  check(api.getAllocationRef(root_two, &root_two_id, &root_two_generation) == VG_SUCCESS,
        "get second same-Node root ref");
  VgCodeObject same_node_code = load(api, device,
                                     atomic_add_module(same_node_id, same_node_generation, 1),
                                     "load same-Node CodeObject");
  VgNodeRef same_node_ref{};
  VgNode same_node = create_node(api, same_node_code, "same-node", &same_node_ref,
                                 "create same-Node fixture");
  VgTaskGraphBuilder same_node_builder = nullptr;
  check(api.createTaskGraphBuilder(device, &builder_desc, &same_node_builder) == VG_SUCCESS,
        "create same-Node graph builder");
  VgTaskRecord same_node_tasks[2]{};
  same_node_tasks[0].node = same_node_ref;
  same_node_tasks[0].root = root_one_id;
  same_node_tasks[0].root_generation = root_one_generation;
  same_node_tasks[0].shape = {1, 1, 1, 0};
  same_node_tasks[0].payload_size = 4;
  same_node_tasks[0].payload_or_offset = 16;
  same_node_tasks[1].node = same_node_ref;
  same_node_tasks[1].root = root_two_id;
  same_node_tasks[1].root_generation = root_two_generation;
  same_node_tasks[1].shape = {7, 2, 3, 1};
  same_node_tasks[1].payload_size = 8;
  same_node_tasks[1].payload_or_offset = 32;
  VgTaskId same_node_ids[2]{};
  check(api.taskGraphAppend(same_node_builder, same_node_tasks, 2, same_node_ids) == VG_SUCCESS,
        "append two differently shaped same-Node tasks");
  check(api.taskGraphAddDependency(same_node_builder, same_node_ids[0], same_node_ids[1]) == VG_SUCCESS,
        "order two atomic same-Node tasks");
  VgTaskGraph same_node_graph = nullptr;
  check(api.sealTaskGraph(same_node_builder, &seal_desc, &same_node_graph) == VG_SUCCESS,
        "seal same-Node graph");
  VgExecutionEnvelopeDesc same_node_envelope_desc = init<VgExecutionEnvelopeDesc>(VG_STRUCTURE_EXECUTION_ENVELOPE_DESC);
  same_node_envelope_desc.arena = arena;
  same_node_envelope_desc.allowed_nodes = &same_node_ref;
  same_node_envelope_desc.allowed_node_count = 1;
  VgExecutionEnvelope same_node_envelope = nullptr;
  check(api.createExecutionEnvelope(device, &same_node_envelope_desc, &same_node_envelope) == VG_SUCCESS,
        "create same-Node envelope");
  VgSubmitDesc same_node_submit = submit_desc;
  same_node_submit.graph = same_node_graph;
  same_node_submit.envelope = same_node_envelope;
  VgSubmission same_node_submission = nullptr;
  check(api.submit(device, &same_node_submit, &same_node_submission) == VG_SUCCESS,
        "submit two same-Node tasks");
  int64_t same_node_result = 0;
  check(api.readAllocation(arena, same_node_value, 0, &same_node_result, sizeof(same_node_result)) == VG_SUCCESS &&
            same_node_result == 2,
        "both same-Node Tasks execute independently rather than being collapsed");
  api.destroySubmission(same_node_submission);
  api.destroyExecutionEnvelope(same_node_envelope);
  api.destroyTaskGraph(same_node_graph);
  api.destroyTaskGraphBuilder(same_node_builder);
  api.destroyNode(same_node);
  api.destroyCodeObject(same_node_code);

  // A Node destroyed before acceptance stales the graph reference; live host
  // CodeObject handles are not sufficient authority to resurrect it.
  VgNodeRef stale_ref{};
  VgNode stale_node = create_node(api, code_a, "stale", &stale_ref, "create stale-node fixture");
  VgTaskGraphBuilder stale_builder = nullptr;
  check(api.createTaskGraphBuilder(device, &builder_desc, &stale_builder) == VG_SUCCESS,
        "create stale-node graph builder");
  VgTaskRecord stale_task{};
  stale_task.node = stale_ref;
  stale_task.root = intermediate_id;
  stale_task.root_generation = intermediate_generation;
  stale_task.shape = {1, 1, 1, 0};
  VgTaskId stale_id{};
  check(api.taskGraphAppend(stale_builder, &stale_task, 1, &stale_id) == VG_SUCCESS,
        "append live stale-node fixture");
  VgTaskGraph stale_graph = nullptr;
  check(api.sealTaskGraph(stale_builder, &seal_desc, &stale_graph) == VG_SUCCESS, "seal stale-node graph");
  api.destroyNode(stale_node);
  VgSubmitDesc stale_submit = submit_desc;
  stale_submit.graph = stale_graph;
  VgSubmission rejected = nullptr;
  check(api.submit(device, &stale_submit, &rejected) == VG_ERROR_STALE_HANDLE,
        "submit rejects graph whose Node generation became stale before acceptance");

  // NodeRefs cannot cross a Device capability namespace, even when both
  // devices happen to expose the same reference adapter.
  VgDevice device_two = nullptr;
  check(api.createDevice(adapter, &device_desc, &device_two) == VG_SUCCESS, "create second device");
  VgCodeObject foreign_code = load(api, device_two, producer, "load second-device CodeObject");
  VgNodeRef foreign_ref{};
  VgNode foreign_node = create_node(api, foreign_code, "foreign", &foreign_ref, "create second-device Node");
  VgTaskGraphBuilder foreign_builder = nullptr;
  check(api.createTaskGraphBuilder(device, &builder_desc, &foreign_builder) == VG_SUCCESS,
        "create cross-device graph builder");
  VgTaskRecord foreign_task{};
  foreign_task.node = foreign_ref;
  foreign_task.root = intermediate_id;
  foreign_task.root_generation = intermediate_generation;
  foreign_task.shape = {1, 1, 1, 0};
  VgTaskId foreign_id{};
  check(api.taskGraphAppend(foreign_builder, &foreign_task, 1, &foreign_id) == VG_ERROR_INVALID_ARGUMENT,
        "cross-device NodeRef is rejected");

  api.destroyTaskGraphBuilder(foreign_builder);
  api.destroyNode(foreign_node);
  api.destroyCodeObject(foreign_code);
  api.destroyDevice(device_two);
  api.destroyTaskGraph(stale_graph);
  api.destroyTaskGraphBuilder(stale_builder);
  api.destroyExecutionEnvelope(envelope);
  api.destroyTaskGraph(graph);
  api.destroyTaskGraphBuilder(builder);
  api.destroyNode(node_b);
  api.destroyNode(node_a);
  api.destroyCodeObject(code_a);
  api.destroyArena(arena);
  api.destroyAddressDomain(domain);
  api.destroyDevice(device);
  api.closeAdapter(adapter);
  api.destroyRuntime(runtime);
  return ok ? 0 : 1;
}
