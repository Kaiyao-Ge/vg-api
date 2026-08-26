#include "backends/device_hal.h"

#include "ir/ir.h"
#include "ir/json.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace vg::hal {

void LoweringReport::add(std::string operation, LoweringClass classification,
                         uint64_t count, uint64_t bytes, std::string reason) {
  events.push_back({std::move(operation), classification, count, bytes, std::move(reason)});
}

uint64_t LoweringReport::count(LoweringClass classification) const {
  uint64_t total = 0;
  for (const auto& event : events) if (event.classification == classification) total += event.count;
  return total;
}

bool LoweringReport::has_hidden_host_wait() const {
  return std::ranges::any_of(events, [](const LoweringEvent& event) {
    return event.classification == LoweringClass::HostAssisted;
  });
}

std::string LoweringReport::canonical_json() const {
  json::Value::Array serialized;
  for (const auto& event : events) serialized.emplace_back(json::Value(json::Value::Object{
      {"bytes", json::Value(static_cast<int64_t>(event.bytes))},
      {"classification", json::Value(static_cast<int64_t>(event.classification))},
      {"count", json::Value(static_cast<int64_t>(event.count))},
      {"operation", json::Value(event.operation)},
      {"reason", json::Value(event.reason)}}));
  return json::canonical(json::Value(json::Value::Object{
      {"abi_version", json::Value(static_cast<int64_t>(abi_version))},
      {"backend", json::Value(static_cast<int64_t>(backend))},
      {"diagnostic", json::Value(diagnostic)},
      {"events", json::Value(std::move(serialized))},
      {"supported", json::Value(static_cast<int64_t>(supported ? 1 : 0))}}));
}

namespace {
bool validate_representation_requests(const std::vector<RepresentationRequest>& requests, std::string* error) {
  for (size_t index = 0; index < requests.size(); ++index) {
    const auto& request = requests[index];
    const std::string label = "representation request " + std::to_string(index);
    std::string view_error;
    if (!request.view.valid(&view_error)) {
      if (error) *error = label + " names a CanonicalView that cannot describe a Region: " + view_error;
      return false;
    }
    if (request.target_kind == core::FacetKind::Address || request.target_kind == core::FacetKind::Transfer) {
      if (error)
        *error = label + " targets an AddressFacet or TransferFacet, which name how an existing "
                         "representation is reached rather than a representation a transform can produce; "
                         "a Stage 5 target must be a Sample, Storage, or Attachment facet";
      return false;
    }
    if (request.consume_input) {
      if (const char* unmet = request.consume_proof.first_unmet()) {
        if (error) *error = label + " asks for ConsumeInput but its proof is incomplete: " + unmet;
        return false;
      }
    }
    for (size_t earlier = 0; earlier < index; ++earlier) {
      if (requests[earlier].view.allocation != request.view.allocation) continue;
      if (error)
        *error = label + " and representation request " + std::to_string(earlier) +
                 " both transform the representation of allocation " + std::to_string(request.view.allocation) +
                 "; one submission must not race two transforms of a single allocation";
      return false;
    }
  }
  return true;
}

bool validate_tier2_select(const ExecutionPlan& plan, std::string* error) {
  if (!plan.request_tier2_select) return true;
  if (plan.authorized_node_classes.size() < 2) {
    if (error) *error = "tier2 select requires at least two authorized node classes";
    return false;
  }
  if (plan.task_graph.tasks().empty()) {
    if (error) *error = "tier2 select requires a published task graph";
    return false;
  }
  for (size_t i = 0; i < plan.authorized_node_classes.size(); ++i) {
    for (size_t j = i + 1; j < plan.authorized_node_classes.size(); ++j) {
      if (plan.authorized_node_classes[i] == plan.authorized_node_classes[j]) {
        if (error) *error = "tier2 authorized node classes must be unique";
        return false;
      }
    }
  }
  return true;
}
}  // namespace

bool ExecutionPlan::validate(std::string* error) const {
  if (abi_version != kDeviceHalAbiVersion) { if (error) *error = "execution plan ABI version is unsupported"; return false; }
  if (capabilities.abi_version != kDeviceHalAbiVersion) { if (error) *error = "capability snapshot ABI version is unsupported"; return false; }
  if (user_raster_shader.has_value()) {
    // F3 (ADR-043 Decision #4) v1 scope cut: a restricted-import MSL
    // submission carries no linear IR to verify (module stays default), and
    // may only contain raster tasks -- no mixed compute+raster.
    if (user_raster_shader->vertex_abi != ir::kRasterVertexAbiXyzuvPackedV1) {
      if (error) *error = "a user_raster_shader submission requires vertex_abi vg.raster.vertex.xyzuv-packed/v1";
      return false;
    }
    for (const auto& task : task_graph.tasks()) {
      if (task.kind != core::TaskKind::Raster) {
        if (error) *error = "a user_raster_shader submission may only contain raster tasks";
        return false;
      }
    }
  } else {
    const auto verification = ir::verify(module);
    if (!verification.ok) { if (error) *error = verification.message; return false; }
  }
  if (!capabilities.supports(Capability::LinearAddress)) { if (error) *error = "linear address capability is unsupported"; return false; }
  if (timeline_signal != 0 && timeline_signal <= timeline_wait) { if (error) *error = "timeline signal does not advance past wait"; return false; }
  if (!published && !task_graph.tasks().empty()) { if (error) *error = "execution plan contains unpublished tasks"; return false; }
  if (published && !task_graph.tasks().empty() && !task_graph.validate_execution(error)) { return false; }
  if (!validate_representation_requests(representation_requests, error)) return false;
  if (working_set_budget.has_value() && working_set_lease.has_value() &&
      working_set_budget->has_limit && working_set_lease->byte_limit > working_set_budget->byte_limit) {
    if (error) *error = "working-set lease exceeds the plan's working-set budget";
    return false;
  }
  if (pending_overflow.has_value() && !pending_overflow->valid(error)) return false;
  return validate_tier2_select(*this, error);
}

bool ExecutionPlan::graph_epoch_matches(const core::Arena& arena, std::string* error) const {
  if (task_graph.tasks().empty()) { return true; }
  if (graph_epoch != arena.topology_epoch()) {
    if (error) *error = "execution plan graph epoch does not match arena topology";
    return false;
  }
  return true;
}

bool run_representation_stage(const std::vector<RepresentationRequest>& requests,
                             core::Arena& arena, core::FacetPool& pool,
                             const std::function<bool(const RepresentationRequest&, core::FacetRef,
                                                      RepresentationTransformCost*,
                                                      std::string*)>& physical,
                             Submission* submission, std::string* error) {
  if (submission == nullptr) { if (error) *error = "submission output is required"; return false; }
  if (requests.empty()) return true;

  submission->representation_epoch = core::RepresentationEpoch{};
  submission->representation_facets.clear();
  submission->retired_facet_count = 0;
  submission->consumed_allocation_count = 0;
  submission->old_backing_bytes = 0;
  submission->new_backing_bytes = 0;
  submission->temporary_bytes = 0;
  submission->completion_delay_ns = 0;
  submission->released_backing_bytes = 0;

  const auto fail = [&](const std::string& message) {
    submission->representation_epoch = core::RepresentationEpoch{};
    submission->representation_facets.clear();
    submission->retired_facet_count = 0;
    submission->consumed_allocation_count = 0;
    submission->old_backing_bytes = 0;
    submission->new_backing_bytes = 0;
    submission->temporary_bytes = 0;
    submission->completion_delay_ns = 0;
    submission->released_backing_bytes = 0;
    submission->report.add("representation_transform", LoweringClass::Unsupported, 1, 0, message);
    if (error) *error = message;
    return false;
  };

  core::RepresentationEpochBuilder builder(&arena);
  std::vector<core::FacetRef> facets;
  std::vector<LoweringEvent> events;
  uint64_t old_backing_bytes = 0;
  uint64_t new_backing_bytes = 0;
  uint64_t temporary_bytes = 0;
  uint64_t heap_fragmentation_bytes = 0;
  uint64_t completion_delay_ns = 0;
  uint64_t released_backing_bytes = 0;
  uint32_t retired_facets = 0;
  uint32_t consumed_allocations = 0;

  for (size_t index = 0; index < requests.size(); ++index) {
    const RepresentationRequest& request = requests[index];
    const std::string label = "representation request " + std::to_string(index);
    const core::Allocation* allocation =
        arena.lookup(core::PointerRef{request.view.allocation, request.view.allocation_generation});
    if (allocation == nullptr)
      return fail(label + " names allocation " + std::to_string(request.view.allocation) +
                  " at generation " + std::to_string(request.view.allocation_generation) +
                  ", which is not active in this Arena");
    const uint64_t superseded_bytes = allocation->bytes.size();
    // ConsumeInput is only legal once there is no external reference to the
    // old representation (02 §4.2). A FacetRef is such a reference, and
    // retire_stale() after transform() would erase a token-only hold before
    // consume_representation() could see it. Check the live epoch first.
    if (request.consume_input &&
        pool.references(core::RepresentationRef{request.view.allocation, request.view.allocation_generation, allocation->representation_epoch})) {
      return fail(label + " asked for ConsumeInput, but a facet token still names the old representation");
    }

    uint32_t new_epoch = 0;
    std::string transform_error;
    if (!arena.transform(request.view.allocation, request.view.allocation_generation, &new_epoch,
                         &transform_error))
      return fail(label + " could not publish a new RepresentationEpoch for allocation " +
                  std::to_string(request.view.allocation) + ": " + transform_error);
    retired_facets += static_cast<uint32_t>(pool.retire_stale(arena));

    // Acquire before the physical step: the facet slot is the handle a backend
    // registers its new resource against (06 §6.4), so the callback needs the
    // ref it is about to fill, not one produced afterwards.
    core::FacetRef facet{};
    std::string acquire_error;
    if (!pool.acquire(arena, request.view, request.target_kind, &facet, &acquire_error))
      return fail(label + " could not acquire its target facet at RepresentationEpoch " +
                  std::to_string(new_epoch) + ": " + acquire_error);

    RepresentationTransformCost cost;
    if (physical) {
      std::string physical_error;
      if (!physical(request, facet, &cost, &physical_error))
        return fail(label + " could not transform allocation " + std::to_string(request.view.allocation) +
                    " into the requested facet representation: " + physical_error);
    }
    const auto consume_eligible_at = std::chrono::steady_clock::now();

    std::string epoch_error;
    if (!builder.add_facet(arena, pool, facet, &epoch_error))
      return fail(label + " could not freeze its facet into the submission's RepresentationEpoch: " +
                  epoch_error);
    facets.push_back(facet);

    old_backing_bytes += superseded_bytes;
    new_backing_bytes += cost.new_backing_bytes;
    temporary_bytes += cost.temporary_bytes;
    heap_fragmentation_bytes += cost.heap_fragmentation_bytes;
    events.push_back({"representation_transform",
                      cost.used_device_optimal ? LoweringClass::DevicePass : LoweringClass::Serialized,
                      1, cost.new_backing_bytes,
                      cost.used_device_optimal
                          ? "transform published a new RepresentationEpoch through a device-optimal pass"
                          : "transform published a new RepresentationEpoch with no device-optimal pass taken"});

    if (!request.consume_input) continue;
    // Refused, not silently skipped: consuming a transform that produced no
    // storage distinct from what it superseded would release the only copy of
    // the bytes the facet just published, and reporting it as a watermark
    // reduction would be a fabricated saving (10 §12).
    if (!cost.distinct_backing)
      return fail(label + " asked for ConsumeInput, but this backend's transform of allocation " +
                  std::to_string(request.view.allocation) +
                  " produced no backing distinct from the one it supersedes, so there is nothing "
                  "the consume could release");
    uint64_t released = 0;
    std::string consume_error;
    if (!arena.consume_representation(request.view.allocation, request.view.allocation_generation,
                                      new_epoch, request.consume_proof, &released, &consume_error))
      return fail(label + " asked for ConsumeInput and it was refused: " + consume_error);
    completion_delay_ns += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                            consume_eligible_at)
            .count());
    released_backing_bytes += released;
    ++consumed_allocations;
    events.push_back({"consume_input", LoweringClass::Direct, 1, released,
                      "ConsumeInput released the superseded backing of allocation " +
                          std::to_string(request.view.allocation) +
                          " at once instead of holding it until command-buffer completion"});
  }

  core::RepresentationEpoch epoch;
  std::string seal_error;
  if (!builder.seal(&epoch, &seal_error))
    return fail("Stage 5 could not seal its RepresentationEpoch: " + seal_error);

  submission->representation_epoch = std::move(epoch);
  submission->representation_facets = std::move(facets);
  submission->retired_facet_count = retired_facets;
  submission->consumed_allocation_count = consumed_allocations;
  submission->old_backing_bytes = old_backing_bytes;
  submission->new_backing_bytes = new_backing_bytes;
  submission->temporary_bytes = temporary_bytes;
  submission->completion_delay_ns = completion_delay_ns;
  submission->released_backing_bytes = released_backing_bytes;
  submission->report.heap_fragmentation_bytes += heap_fragmentation_bytes;
  for (auto& event : events) submission->report.events.push_back(std::move(event));
  return true;
}

}  // namespace vg::hal
