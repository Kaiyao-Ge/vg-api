// TASK-D3 / ADR-037: working-set budget is this-submit residency, not the
// address graph. Small allocations only (16–64 bytes) -- never near RAM.
#include "backends/device_hal.h"
#include "backends/reference/reference_device_hal.h"
#include "compiler/compiler.h"

#include <cassert>
#include <iostream>
#include <string>

namespace {

const vg::hal::LoweringEvent* find_event(const vg::hal::LoweringReport& report, const std::string& name) {
  for (const auto& event : report.events) {
    if (event.operation == name) return &event;
  }
  return nullptr;
}

bool has_proxy_reason(const vg::hal::LoweringEvent& event) {
  return event.reason.find("proxy") != std::string::npos;
}

vg::ir::Module store_module() {
  auto compiled = vg::compiler::compile_c_like("@node @effects store(1,0,4,7)");
  assert(compiled.ok);
  return compiled.module;
}

vg::hal::ExecutionPlan base_plan(vg::hal::DeviceHal& device, const vg::ir::Module& module) {
  vg::hal::ExecutionPlan plan;
  plan.capabilities = device.capabilities();
  plan.module = module;
  plan.published = true;
  return plan;
}

void require_working_set_events(const vg::hal::LoweringReport& report, uint64_t requested,
                                bool committed) {
  const auto* requested_event = find_event(report, "working_set_requested");
  const auto* committed_event = find_event(report, "working_set_committed");
  const auto* proxy_event = find_event(report, "working_set_proxy");
  const auto* sparse_event = find_event(report, "working_set_sparse");
  assert(requested_event != nullptr);
  assert(committed_event != nullptr);
  assert(proxy_event != nullptr);
  assert(sparse_event != nullptr);
  assert(requested_event->bytes == requested);
  assert(committed_event->bytes == (committed ? requested : 0));
  assert(proxy_event->bytes == requested);
  assert(has_proxy_reason(*requested_event));
  assert(has_proxy_reason(*committed_event));
  assert(has_proxy_reason(*proxy_event));
  assert(sparse_event->classification == vg::hal::LoweringClass::Unsupported);
}

}  // namespace

int main() {
  auto device = vg::reference::make_device_hal();
  assert(device != nullptr);
  const auto module = store_module();

  uint64_t hand_picked_requested = 0;
  uint64_t universe_requested = 0;

  // Hand-picked lease 16 bytes, budget 64: pass.
  {
    vg::core::Arena arena;
    const auto& first = arena.allocate(16);
    arena.allocate(16);
    auto plan = base_plan(*device, module);
    plan.working_set_budget = vg::core::WorkingSetBudget::limited(64);
    plan.working_set_lease = vg::core::WorkingSetLease{};
    plan.working_set_lease->allocations.push_back({first.id, first.generation});
    plan.working_set_lease->byte_limit = 16;
    plan.working_set_lease->complete = true;
    assert(plan.validate());

    vg::hal::CompiledPlan compiled;
    std::string error;
    assert(device->compile(plan, &compiled, &error));
    vg::hal::Submission submission;
    assert(device->submit(compiled, arena, &submission, &error));
    assert(submission.result.ok);
    const auto* requested_event = find_event(submission.report, "working_set_requested");
    assert(requested_event != nullptr);
    hand_picked_requested = requested_event->bytes;
    assert(hand_picked_requested == 16);
    require_working_set_events(submission.report, 16, true);
    std::cout << "hand-picked requested=" << hand_picked_requested << "\n";
  }

  // Universe (no lease) two 16-byte actives, budget 16: refuse.
  {
    vg::core::Arena arena;
    arena.allocate(16);
    arena.allocate(16);
    auto plan = base_plan(*device, module);
    plan.working_set_budget = vg::core::WorkingSetBudget::limited(16);
    assert(plan.validate());

    vg::hal::CompiledPlan compiled;
    std::string error;
    assert(device->compile(plan, &compiled, &error));
    vg::hal::Submission submission;
    assert(!device->submit(compiled, arena, &submission, &error));
    assert(error == "working-set budget exceeded");
    const auto* requested_event = find_event(submission.report, "working_set_requested");
    assert(requested_event != nullptr);
    universe_requested = requested_event->bytes;
    assert(universe_requested == 32);
    require_working_set_events(submission.report, 32, false);
    std::cout << "universe requested=" << universe_requested << "\n";
  }

  // Whole-arena vs hand-picked: reported requested bytes differ.
  assert(hand_picked_requested != universe_requested);
  std::cout << "whole-arena vs hand-picked differ: " << universe_requested << " != "
            << hand_picked_requested << "\n";

  // Same two-active arena, lease vs Universe, measured on one submit pair.
  {
    vg::core::Arena arena;
    const auto& first = arena.allocate(16);
    arena.allocate(16);
    auto leased = base_plan(*device, module);
    leased.working_set_budget = vg::core::WorkingSetBudget::limited(64);
    leased.working_set_lease = vg::core::WorkingSetLease{};
    leased.working_set_lease->allocations.push_back({first.id, first.generation});
    leased.working_set_lease->byte_limit = 16;
    auto universe = base_plan(*device, module);
    universe.working_set_budget = vg::core::WorkingSetBudget::limited(64);

    vg::hal::Submission leased_submission;
    vg::hal::Submission universe_submission;
    assert(vg::hal::apply_working_set_budget(leased, arena, &leased_submission, nullptr));
    assert(vg::hal::apply_working_set_budget(universe, arena, &universe_submission, nullptr));
    const auto* leased_event = find_event(leased_submission.report, "working_set_requested");
    const auto* universe_event = find_event(universe_submission.report, "working_set_requested");
    assert(leased_event != nullptr && universe_event != nullptr);
    assert(leased_event->bytes == 16);
    assert(universe_event->bytes == 32);
    assert(leased_event->bytes != universe_event->bytes);
  }

  // Default plan (no budget): unchanged success, no working-set events.
  {
    vg::core::Arena arena;
    arena.allocate(16);
    arena.allocate(16);
    auto plan = base_plan(*device, module);
    assert(!plan.working_set_budget.has_value());
    assert(!plan.working_set_lease.has_value());
    vg::hal::CompiledPlan compiled;
    std::string error;
    assert(device->compile(plan, &compiled, &error));
    vg::hal::Submission submission;
    assert(device->submit(compiled, arena, &submission, &error));
    assert(submission.result.ok);
    assert(find_event(submission.report, "working_set_requested") == nullptr);
    assert(find_event(submission.report, "working_set_committed") == nullptr);
    assert(find_event(submission.report, "working_set_proxy") == nullptr);
  }

  // Missing / stale lease names: refuse, never invent a size.
  {
    vg::core::Arena arena;
    arena.allocate(16);
    auto plan = base_plan(*device, module);
    plan.working_set_budget = vg::core::WorkingSetBudget::limited(64);
    plan.working_set_lease = vg::core::WorkingSetLease{};
    plan.working_set_lease->allocations.push_back({99, 1});
    plan.working_set_lease->byte_limit = 16;
    vg::hal::Submission submission;
    std::string error;
    assert(!vg::hal::apply_working_set_budget(plan, arena, &submission, &error));
    assert(error == "working-set lease names a missing or stale allocation");
  }

  // Discovery-lease: real discover_reachable (TASK-D2), not a fake subset.
  // Two 16-byte actives; seed only the first. Reachable requested bytes are
  // 16, while Universe of the same arena is 32.
  {
    vg::core::Arena arena;
    const auto& seed_alloc = arena.allocate(16);
    arena.allocate(16);
    vg::core::DiscoveryResult discovery;
    std::string error;
    assert(vg::core::discover_reachable(arena, {{seed_alloc.id, seed_alloc.generation}}, &discovery,
                                        &error));
    assert(discovery.reachable.size() == 1);
    assert(discovery.result_bytes == 16);

    auto plan = base_plan(*device, module);
    plan.working_set_budget = vg::core::WorkingSetBudget::limited(16);
    plan.working_set_lease = vg::core::WorkingSetLease{};
    for (const auto& ref : discovery.reachable) {
      assert(plan.working_set_lease->add(ref, discovery.reachable, &error));
    }
    plan.working_set_lease->byte_limit = discovery.result_bytes;
    plan.working_set_lease->complete = true;
    assert(plan.validate());

    vg::hal::CompiledPlan compiled;
    assert(device->compile(plan, &compiled, &error));
    vg::hal::Submission submission;
    assert(device->submit(compiled, arena, &submission, &error));
    assert(submission.result.ok);
    require_working_set_events(submission.report, 16, true);
    std::cout << "discovery-lease requested=16 (universe of same arena would be 32)\n";
  }

  return 0;
}
