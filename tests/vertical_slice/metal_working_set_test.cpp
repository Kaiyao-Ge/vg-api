// TASK-D3 / ADR-037 Metal vertical slice: same refuse/pass as the reference
// working-set unit test, plus report events and sparse Unsupported.
// Allocations are 16 bytes only -- never near RAM.
#include "backends/device_hal.h"
#include "backends/metal/metal_device_hal.h"
#include "compiler/compiler.h"
#include "../support/assembled_plan_fixture.h"

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

bool require_working_set_events(const vg::hal::LoweringReport& report, uint64_t requested, bool committed,
                               const char* label) {
  const auto* requested_event = find_event(report, "working_set_requested");
  const auto* committed_event = find_event(report, "working_set_committed");
  const auto* proxy_event = find_event(report, "working_set_proxy");
  const auto* sparse_event = find_event(report, "working_set_sparse");
  if (requested_event == nullptr || committed_event == nullptr || proxy_event == nullptr) {
    std::cerr << label << ": missing working_set_requested/committed/proxy event\n";
    return false;
  }
  if (requested_event->bytes != requested || proxy_event->bytes != requested ||
      committed_event->bytes != (committed ? requested : 0)) {
    std::cerr << label << ": unexpected working-set bytes requested=" << requested_event->bytes
              << " committed=" << committed_event->bytes << " proxy=" << proxy_event->bytes << "\n";
    return false;
  }
  if (!has_proxy_reason(*requested_event) || !has_proxy_reason(*committed_event) ||
      !has_proxy_reason(*proxy_event)) {
    std::cerr << label << ": working-set reason must say proxy (no OS residency counter)\n";
    return false;
  }
  if (sparse_event == nullptr || sparse_event->classification != vg::hal::LoweringClass::Unsupported) {
    std::cerr << label << ": working_set_sparse must be present as Unsupported\n";
    return false;
  }
  return true;
}

vg::ir::Module store_module() {
  auto compiled = vg::compiler::compile_c_like("@node @effects store(1,0,4,7)");
  return compiled.module;
}

bool assemble_store_plan(vg::core::Arena& arena, const vg::ir::Module& module, vg::core::PointerRef root,
                         vg::test_support::AssembledPlanFixture* fixture, vg::hal::ExecutionPlan* plan,
                         std::string* error, const vg::core::WorkingSetBudget* budget = nullptr,
                         const vg::core::WorkingSetLease* lease = nullptr,
                         const std::vector<vg::core::PointerRef>* seeds = nullptr,
                         std::optional<vg::core::AccessCertificateMode> mode = std::nullopt) {
  vg::test_support::AssemblyOptions options;
  options.working_set_budget = budget;
  options.working_set_lease = lease;
  options.discovery_seeds = seeds;
  options.certificate_mode = mode;
  if (mode == vg::core::AccessCertificateMode::CertifiedPinned) options.certificate_touched = {root};
  return vg::test_support::assemble_single_node_plan(
      arena, module, {vg::test_support::compute_task(root.allocation, root.generation)}, fixture, plan, error,
      options);
}

}  // namespace

int main() {
  auto metal = vg::metal::make_device_hal();
  if (metal == nullptr) {
    std::cerr << "working-set: no Metal device available on this host\n";
    return 1;
  }

  const auto module = store_module();
  if (module.instructions.empty()) {
    std::cerr << "working-set: compile_c_like fixture failed\n";
    return 1;
  }

  // Hand-picked lease 16 bytes, budget 64: pass.
  {
    vg::core::Arena arena;
    const auto& first = arena.allocate(16);
    arena.allocate(16);
    vg::core::WorkingSetBudget budget = vg::core::WorkingSetBudget::limited(64);
    vg::core::WorkingSetLease lease;
    lease.allocations.push_back({first.id, first.generation});
    lease.byte_limit = 16;
    vg::hal::ExecutionPlan plan;
    vg::test_support::AssembledPlanFixture fixture;
    vg::hal::Submission submission;
    std::string error;
    if (!assemble_store_plan(arena, module, {first.id, first.generation}, &fixture, &plan, &error, &budget,
                             &lease, nullptr, vg::core::AccessCertificateMode::CertifiedPinned)) {
      std::cerr << "working-set: hand-picked assembly failed: " << error << "\n";
      return 1;
    }
    if (!vg::hal::apply_working_set_budget(plan, arena, &submission, &error)) {
      std::cerr << "working-set: hand-picked lease should pass: " << error << "\n";
      return 1;
    }
    if (!require_working_set_events(submission.report, 16, true, "hand-picked")) return 1;
    std::cout << "metal hand-picked requested=16\n";
  }

  // Universe (no lease) two 16-byte actives, budget 16: refuse.
  {
    vg::core::Arena arena;
    const auto& first = arena.allocate(16);
    arena.allocate(16);
    vg::core::WorkingSetBudget budget = vg::core::WorkingSetBudget::limited(16);
    vg::hal::ExecutionPlan plan;
    vg::test_support::AssembledPlanFixture fixture;
    std::string error;
    if (assemble_store_plan(arena, module, {first.id, first.generation}, &fixture, &plan, &error,
                            &budget, nullptr, nullptr,
                            vg::core::AccessCertificateMode::Universe)) {
      std::cerr << "working-set: universe over budget must be rejected during assembly\n";
      return 1;
    }
    if (error != "working-set budget exceeded") {
      std::cerr << "working-set: expected 'working-set budget exceeded', got '" << error << "'\n";
      return 1;
    }
    std::cout << "metal universe requested=32 (rejected during assembly)\n";
  }

  // Default plan (no budget): Metal submit unchanged success.
  {
    vg::core::Arena arena;
    const auto& first = arena.allocate(16);
    vg::hal::ExecutionPlan plan;
    vg::test_support::AssembledPlanFixture fixture;
    vg::hal::CompiledPlan compiled;
    std::string error;
    if (!assemble_store_plan(arena, module, {first.id, first.generation}, &fixture, &plan, &error)) {
      std::cerr << "working-set: default assembly failed: " << error << "\n";
      return 1;
    }
    if (!metal->compile(plan, &compiled, &error)) {
      std::cerr << "working-set: default compile failed: " << error << "\n";
      return 1;
    }
    vg::hal::Submission submission;
    if (!metal->submit(compiled, arena, &submission, &error) || !submission.result.ok) {
      std::cerr << "working-set: default submit failed: " << error << " "
                << submission.result.message << "\n";
      return 1;
    }
    if (find_event(submission.report, "working_set_requested") != nullptr) {
      std::cerr << "working-set: default plan must not emit working-set events\n";
      return 1;
    }
  }

  // Discovery-lease: real discover_reachable, not a fake subset.
  {
    vg::core::Arena arena;
    const auto& seed_alloc = arena.allocate(16);
    arena.allocate(16);
    vg::core::DiscoveryResult discovery;
    std::string error;
    if (!vg::core::discover_reachable(arena, {{seed_alloc.id, seed_alloc.generation}}, &discovery,
                                      &error) ||
        discovery.result_bytes != 16) {
      std::cerr << "working-set: discover_reachable failed: " << error << "\n";
      return 1;
    }
    vg::core::WorkingSetBudget budget = vg::core::WorkingSetBudget::limited(16);
    vg::core::WorkingSetLease lease;
    for (const auto& ref : discovery.reachable) {
      if (!lease.add(ref, discovery.reachable, &error)) {
        std::cerr << "working-set: discovery lease add failed: " << error << "\n";
        return 1;
      }
    }
    lease.byte_limit = discovery.result_bytes;
    lease.complete = true;
    const std::vector<vg::core::PointerRef> seeds{{seed_alloc.id, seed_alloc.generation}};
    vg::hal::ExecutionPlan plan;
    vg::test_support::AssembledPlanFixture fixture;
    vg::hal::Submission submission;
    if (!assemble_store_plan(arena, module, {seed_alloc.id, seed_alloc.generation}, &fixture, &plan, &error,
                             &budget, &lease, &seeds,
                             vg::core::AccessCertificateMode::DiscoverThenLease)) {
      std::cerr << "working-set: discovery-lease assembly failed: " << error << "\n";
      return 1;
    }
    if (!vg::hal::apply_working_set_budget(plan, arena, &submission, &error)) {
      std::cerr << "working-set: discovery-lease should pass: " << error << "\n";
      return 1;
    }
    if (!require_working_set_events(submission.report, 16, true, "discovery-lease")) return 1;
    std::cout << "metal discovery-lease requested=16\n";
  }

  std::cout << "working-set: ok\n";
  return 0;
}
