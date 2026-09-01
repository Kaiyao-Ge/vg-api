#include "backends/device_hal.h"

#include "core/scene_root.h"
#include "ir/ir.h"
#include "ir/json.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace vg::hal {

namespace {
bool preflight_fail(const core::ExecutionPlan& plan, const CapabilitySnapshot& capabilities,
                    CompiledPlan* compiled, std::string message, std::string* error) {
  if (compiled) {
    // A reused output must never expose packages or effect state produced by
    // an earlier successful compile after this compile was refused.
    *compiled = CompiledPlan{};
    compiled->abi_version = kDeviceHalAbiVersion;
    compiled->plan = plan;
    compiled->report = {};
    compiled->report.backend = capabilities.backend;
    compiled->report.supported = false;
    compiled->report.diagnostic = message;
    compiled->report.add("stage6_preflight", LoweringClass::Unsupported, 1, 0, message);
  }
  if (error) *error = std::move(message);
  return false;
}

bool pointer_ref_less(core::PointerRef left, core::PointerRef right) {
  return left.allocation != right.allocation ? left.allocation < right.allocation
                                             : left.generation < right.generation;
}

bool same_pointer_ref(core::PointerRef left, core::PointerRef right) {
  return left.allocation == right.allocation && left.generation == right.generation;
}

bool facet_use_less(const core::FacetLifetimeUse& left, const core::FacetLifetimeUse& right) {
  if (left.ref.index != right.ref.index) return left.ref.index < right.ref.index;
  if (left.ref.generation != right.ref.generation)
    return left.ref.generation < right.ref.generation;
  return static_cast<uint32_t>(left.kind) < static_cast<uint32_t>(right.kind);
}

bool same_facet_ref(core::FacetRef left, core::FacetRef right) {
  return left.index == right.index && left.generation == right.generation;
}

void sort_unique_allocations(std::vector<core::PointerRef>* refs) {
  std::sort(refs->begin(), refs->end(), pointer_ref_less);
  refs->erase(std::unique(refs->begin(), refs->end(), same_pointer_ref), refs->end());
}

bool sort_unique_facets(std::vector<core::FacetLifetimeUse>* uses, std::string* error) {
  std::sort(uses->begin(), uses->end(), facet_use_less);
  for (size_t index = 1; index < uses->size(); ++index) {
    if (!same_facet_ref((*uses)[index - 1].ref, (*uses)[index].ref)) continue;
    if ((*uses)[index - 1].kind != (*uses)[index].kind) {
      if (error) *error = "one sealed FacetRef has incompatible lifetime-use kinds";
      return false;
    }
  }
  uses->erase(std::unique(uses->begin(), uses->end(), [](const auto& left, const auto& right) {
                return same_facet_ref(left.ref, right.ref);
              }),
              uses->end());
  return true;
}
}  // namespace

bool preflight_stage6(const core::ExecutionPlan& plan, const CapabilitySnapshot& capabilities,
                      BackendKind expected_backend, CompiledPlan* compiled, std::string* error) {
  if (!compiled) {
    if (error) *error = "compiled plan output is required";
    return false;
  }
  if (capabilities.abi_version != kDeviceHalAbiVersion)
    return preflight_fail(plan, capabilities, compiled, "HAL capability snapshot ABI version is unsupported", error);
  if (capabilities.backend != expected_backend)
    return preflight_fail(plan, capabilities, compiled, "HAL capability snapshot backend does not match adapter", error);
  if (!plan.assembled || !plan.capability_requirements_derived)
    return preflight_fail(plan, capabilities, compiled,
                          "Stage6 requires sealed capability requirements from the core assembler", error);
  if (plan.requested_certificate_mode.has_value() &&
      (!plan.access_plan_derived || !plan.access_certificate.has_value()))
    return preflight_fail(plan, capabilities, compiled,
                          "Stage6 requires sealed access-planning facts from the core assembler", error);
  if ((plan.working_set_budget.has_value() || plan.working_set_lease.has_value()) &&
      (!plan.access_plan_derived || (plan.working_set_budget.has_value() && !plan.working_set_budget_checked)))
    return preflight_fail(plan, capabilities, compiled,
                          "Stage6 requires sealed working-set facts from the core assembler", error);
  for (const auto requirement : plan.required_capabilities) {
    Capability capability{};
    switch (requirement) {
      case core::CapabilityRequirement::LinearAddress: capability = Capability::LinearAddress; break;
      case core::CapabilityRequirement::TaskPublication: capability = Capability::TaskPublication; break;
      case core::CapabilityRequirement::Timeline: capability = Capability::Timeline; break;
      case core::CapabilityRequirement::EffectDag: capability = Capability::EffectDag; break;
      case core::CapabilityRequirement::CaptureReplay: capability = Capability::CaptureReplay; break;
      case core::CapabilityRequirement::IndirectTier1: capability = Capability::IndirectTier1; break;
      case core::CapabilityRequirement::IndirectTier2Select: capability = Capability::IndirectTier2Select; break;
      case core::CapabilityRequirement::IndexedBinding: capability = Capability::IndexedBinding; break;
      case core::CapabilityRequirement::Raster: capability = Capability::Raster; break;
      case core::CapabilityRequirement::RepresentationTransform: capability = Capability::RepresentationTransform; break;
      case core::CapabilityRequirement::CheckedFacetGeneration:
        if (!capabilities.validation_available)
          return preflight_fail(plan, capabilities, compiled,
                                "CheckedNative validation profile is unsupported by this adapter", error);
        capability = Capability::CheckedFacetGeneration;
        break;
      case core::CapabilityRequirement::UserShaderImport: capability = Capability::UserShaderImport; break;
      case core::CapabilityRequirement::ReferenceStrict:
        if (expected_backend != BackendKind::Reference || !capabilities.validation_available)
          return preflight_fail(plan, capabilities, compiled,
                                "ReferenceStrict validation profile is supported only by the validating Reference backend", error);
        continue;
    }
    if (!capabilities.supports(capability))
      return preflight_fail(plan, capabilities, compiled,
                            std::string("required HAL capability is unsupported: ") +
                                core::capability_requirement_name(requirement), error);
  }
  return true;
}

bool validate_stage7_compiled_plan(const CompiledPlan& compiled,
                                   BackendKind expected_backend,
                                   std::string* error) {
  if (compiled.abi_version != kDeviceHalAbiVersion) {
    if (error) *error = "compiled plan ABI version is unsupported";
    return false;
  }
  if (compiled.report.backend != expected_backend) {
    if (error) *error = "compiled plan backend does not match adapter";
    return false;
  }
  return true;
}

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
bool commit_representation_operations(const core::ExecutionPlan& plan,
                             const std::vector<CompiledPlan::PhysicalRepresentationOperation>& operations,
                             core::Arena& arena, core::FacetPool& pool,
                             const std::function<bool(const core::RepresentationSemanticPlanItem&, const CompiledPlan::PhysicalRepresentationOperation&, core::FacetRef,
                                                      RepresentationTransformCost*,
                                                      std::string*)>& physical,
                             Submission* submission, std::string* error) {
  if (submission == nullptr) { if (error) *error = "submission output is required"; return false; }
  if (plan.representation_plan.empty()) return true;
  if (!plan.representation_plan_derived || operations.size() != plan.representation_plan.size()) {
    if (error) *error = "compiled representation operations do not match the frozen semantic plan";
    return false;
  }

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

  for (size_t index = 0; index < plan.representation_plan.size(); ++index) {
    const auto& request = plan.representation_plan[index];
    const auto& operation = operations[index];
    if (operation.semantic_order != request.transform_order || operation.operation == CompiledPlan::RepresentationOperation::Unsupported)
      return fail("compiled representation operation is unsupported or has a mismatched frozen order");
    const std::string label = "representation request " + std::to_string(index);
    const core::Allocation* allocation =
        arena.lookup(core::PointerRef{request.view.allocation, request.view.allocation_generation});
    if (allocation == nullptr || allocation->representation_epoch != request.source_representation_epoch)
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
    if (new_epoch != request.target_representation_epoch || !pool.acquire(arena, request.view, request.target_kind, &facet, &acquire_error))
      return fail(label + " could not acquire its target facet at RepresentationEpoch " +
                  std::to_string(new_epoch) + ": " + acquire_error);

    RepresentationTransformCost cost;
    if (physical) {
      std::string physical_error;
      if (!physical(request, operation, facet, &cost, &physical_error))
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

SubmissionLifetimeHold::SubmissionLifetimeHold(SubmissionLifetimeHold&& other) noexcept {
  *this = std::move(other);
}

SubmissionLifetimeHold& SubmissionLifetimeHold::operator=(SubmissionLifetimeHold&& other) noexcept {
  if (this == &other) return *this;
  release();
  arena_ = std::exchange(other.arena_, nullptr);
  pool_ = std::exchange(other.pool_, nullptr);
  allocation_inventory_ = std::move(other.allocation_inventory_);
  facet_inventory_ = std::move(other.facet_inventory_);
  representation_kinds_ = std::move(other.representation_kinds_);
  acquired_allocations_ = std::move(other.acquired_allocations_);
  acquired_facets_ = std::move(other.acquired_facets_);
  held_ = std::exchange(other.held_, false);
  return *this;
}

SubmissionLifetimeHold::~SubmissionLifetimeHold() { release(); }

bool SubmissionLifetimeHold::prepare(const core::ExecutionPlan& plan, core::Arena& arena,
                                     core::FacetPool& pool, std::string* error) {
  if (prepared() || held_) {
    if (error) *error = "submission lifetime hold is already prepared";
    return false;
  }
  if (!plan.assembled || !plan.lifetime_plan_derived) {
    if (error) *error = "submission lifetime hold requires sealed core lifetime facts";
    return false;
  }

  std::vector<core::PointerRef> allocations = plan.touched_allocations;
  std::vector<core::FacetLifetimeUse> facets = plan.lifetime_facets;
  std::vector<core::FacetKind> representation_kinds;
  representation_kinds.reserve(plan.representation_plan.size());
  for (const auto& item : plan.representation_plan) {
    allocations.push_back({item.view.allocation, item.view.allocation_generation});
    representation_kinds.push_back(item.target_kind);
  }
  if (!sort_unique_facets(&facets, error)) return false;

  for (const auto& use : facets) {
    core::FacetStatus status = core::FacetStatus::Ok;
    const core::FacetSlot* slot = pool.lookup(arena, use.ref, &status);
    if (slot == nullptr) {
      if (error) {
        *error = "sealed lifetime facet " + std::to_string(use.ref.index) + ":" +
                 std::to_string(use.ref.generation) + " is unavailable: " + core::to_string(status);
      }
      return false;
    }
    if (slot->kind != use.kind) {
      if (error) {
        *error = "sealed lifetime facet " + std::to_string(use.ref.index) + ":" +
                 std::to_string(use.ref.generation) + " has the wrong facet kind";
      }
      return false;
    }
    const core::PointerRef backing{slot->view.allocation, slot->view.allocation_generation};
    for (const auto& item : plan.representation_plan) {
      if (backing.allocation == item.view.allocation &&
          backing.generation == item.view.allocation_generation) {
        if (error) {
          *error = "a same-submit representation transform would invalidate a sealed Task FacetRef before Stage 7";
        }
        return false;
      }
    }
    allocations.push_back(backing);
  }

  sort_unique_allocations(&allocations);
  for (const auto ref : allocations) {
    if (arena.lookup(ref) == nullptr) {
      if (error) {
        *error = "sealed allocation lifetime fact is stale or retired: allocation " +
                 std::to_string(ref.allocation) + " generation " + std::to_string(ref.generation);
      }
      return false;
    }
  }

  arena_ = &arena;
  pool_ = &pool;
  allocation_inventory_ = std::move(allocations);
  facet_inventory_ = std::move(facets);
  representation_kinds_ = std::move(representation_kinds);
  return true;
}

bool SubmissionLifetimeHold::acquire(
    const std::vector<core::FacetRef>& representation_facets, std::string* error) {
  if (!prepared()) {
    if (error) *error = "submission lifetime hold must be prepared before acquisition";
    return false;
  }
  if (held_) {
    if (error) *error = "submission lifetime hold is already acquired";
    return false;
  }
  if (representation_facets.size() != representation_kinds_.size()) {
    if (error) *error = "physical representation outputs do not match the sealed lifetime plan";
    return false;
  }

  std::vector<core::FacetLifetimeUse> facets = facet_inventory_;
  for (size_t index = 0; index < representation_facets.size(); ++index)
    facets.push_back({representation_facets[index], representation_kinds_[index]});
  if (!sort_unique_facets(&facets, error)) return false;

  std::vector<core::PointerRef> allocations = allocation_inventory_;
  for (const auto& use : facets) {
    core::FacetStatus status = core::FacetStatus::Ok;
    const core::FacetSlot* slot = pool_->lookup(*arena_, use.ref, &status);
    if (slot == nullptr) {
      if (error) {
        *error = "final lifetime facet " + std::to_string(use.ref.index) + ":" +
                 std::to_string(use.ref.generation) + " is unavailable: " + core::to_string(status);
      }
      return false;
    }
    if (slot->kind != use.kind) {
      if (error) {
        *error = "final lifetime facet " + std::to_string(use.ref.index) + ":" +
                 std::to_string(use.ref.generation) + " has the wrong facet kind";
      }
      return false;
    }
    allocations.push_back({slot->view.allocation, slot->view.allocation_generation});
  }
  sort_unique_allocations(&allocations);

  acquired_allocations_.clear();
  acquired_facets_.clear();
  for (const auto ref : allocations) {
    if (!arena_->acquire(ref.allocation, ref.generation)) {
      if (error) {
        *error = "could not acquire allocation lifetime hold for allocation " +
                 std::to_string(ref.allocation) + " generation " + std::to_string(ref.generation) +
                 ": stale or retired";
      }
      release();
      return false;
    }
    acquired_allocations_.push_back(ref);
  }
  for (const auto& use : facets) {
    std::string facet_error;
    if (!pool_->begin_gpu_use(*arena_, use.ref, &facet_error)) {
      if (error) {
        *error = "could not acquire facet lifetime hold for facet " +
                 std::to_string(use.ref.index) + ":" + std::to_string(use.ref.generation) +
                 ": " + facet_error;
      }
      release();
      return false;
    }
    acquired_facets_.push_back(use.ref);
  }
  held_ = true;
  return true;
}

void SubmissionLifetimeHold::release() noexcept {
  if (pool_ != nullptr) {
    for (auto it = acquired_facets_.rbegin(); it != acquired_facets_.rend(); ++it)
      (void)pool_->end_gpu_use(*it);
  }
  if (arena_ != nullptr) {
    for (auto it = acquired_allocations_.rbegin(); it != acquired_allocations_.rend(); ++it)
      (void)arena_->release(it->allocation, it->generation);
  }
  acquired_facets_.clear();
  acquired_allocations_.clear();
  held_ = false;
}

}  // namespace vg::hal
