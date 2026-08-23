// TASK-D2 / ADR-036 Metal+reference slice: the same 4-node pointer chain
// as tests/unit/discovery_test.cpp, walked as a HostAssisted host
// round-trip (never DevicePass). Discovered count must be strictly
// smaller than Universe on the same Arena. This is a semantic reachable
// set / proxy, not OS page migration (06 §10).
#include "backends/device_hal.h"
#include "backends/metal/metal_device_hal.h"
#include "backends/reference/reference_device_hal.h"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr size_t kPointerRefWireBytes = sizeof(uint64_t) + sizeof(uint32_t);

void write_ref(vg::core::Allocation& allocation, const vg::core::PointerRef& ref) {
  std::memcpy(allocation.bytes.data(), &ref.allocation, sizeof(ref.allocation));
  std::memcpy(allocation.bytes.data() + sizeof(ref.allocation), &ref.generation, sizeof(ref.generation));
}

size_t active_count(const vg::core::Arena& arena) {
  size_t count = 0;
  for (const auto& [id, allocation] : arena.allocations()) {
    (void)id;
    if (allocation.state == vg::core::ObjectState::Active) ++count;
  }
  return count;
}

const vg::hal::LoweringEvent* find_event(const vg::hal::LoweringReport& report, const char* name) {
  for (const auto& event : report.events) {
    if (event.operation == name) return &event;
  }
  return nullptr;
}

// Four Active allocations, n0->n1->(broken gen 0)->n2->n3. Seed n0
// reaches two nodes. The probe module loads 4 bytes of n0 so compile()
// / submit() have a real package without adding a fifth allocation.
void build_chain(vg::core::Arena& arena, vg::ir::Module* module, vg::core::PointerRef* seed) {
  auto& n0 = arena.allocate(12);
  auto& n1 = arena.allocate(12);
  auto& n2 = arena.allocate(12);
  auto& n3 = arena.allocate(12);
  write_ref(n0, {n1.id, n1.generation});
  write_ref(n1, {n2.id, 0});
  write_ref(n2, {n3.id, n3.generation});
  *seed = {n0.id, n0.generation};

  module->version = 1;
  module->root_schema = "vg.test/discovery-revisit";
  vg::ir::Instruction load;
  load.op = "load";
  load.allocation = n0.id;
  load.generation = n0.generation;
  load.representation_epoch = n0.representation_epoch;
  load.offset = 0;
  load.size = 4;
  module->instructions.push_back(load);
  module->declared_effects.push_back(
      {n0.id, 0, 12, vg::ir::Access::Read, n0.representation_epoch});
}

bool require_discovery(const vg::hal::Submission& submission, size_t universe, const char* label) {
  if (!submission.access_certificate.has_value()) {
    std::cerr << label << ": missing discovered certificate\n";
    return false;
  }
  const auto& certificate = *submission.access_certificate;
  if (certificate.mode != vg::core::AccessCertificateMode::DiscoverThenLease) {
    std::cerr << label << ": certificate mode is not DiscoverThenLease\n";
    return false;
  }
  const size_t discovered = certificate.epoch.references().size();
  if (discovered >= universe) {
    std::cerr << label << ": discovered=" << discovered << " is not < universe=" << universe << "\n";
    return false;
  }
  if (discovered != 2) {
    std::cerr << label << ": expected discovered=2, got " << discovered << "\n";
    return false;
  }
  const auto* event = find_event(submission.report, "discovery");
  if (event == nullptr) {
    std::cerr << label << ": missing discovery report event\n";
    return false;
  }
  if (event->classification != vg::hal::LoweringClass::HostAssisted) {
    std::cerr << label << ": discovery must be HostAssisted, never DevicePass\n";
    return false;
  }
  if (event->classification == vg::hal::LoweringClass::DevicePass) {
    std::cerr << label << ": discovery classified DevicePass\n";
    return false;
  }
  if (certificate.discovery_host_ns == 0 && event->bytes == 0) {
    std::cerr << label << ": discovery reported no host time and no result bytes\n";
    return false;
  }
  std::cout << label << ": discovered=" << discovered << " universe=" << universe
            << " discovery_host_ns=" << certificate.discovery_host_ns
            << " scanned_bytes=" << certificate.scanned_bytes
            << " result_bytes=" << certificate.result_bytes << " HostAssisted\n";
  return true;
}

}  // namespace

int main() {
  auto metal = vg::metal::make_device_hal();
  if (metal == nullptr) {
    std::cerr << "discovery: no Metal device available on this host\n";
    return 1;
  }
  auto reference = vg::reference::make_device_hal();
  if (reference == nullptr) {
    std::cerr << "discovery: reference device missing\n";
    return 1;
  }

  // Metal: real compile/submit of the probe. submit() must call
  // run_discovery_stage (one-line hook in metal_device_hal.mm). If that
  // hook is missing, this path will not attach a discovered certificate.
  {
    vg::core::Arena arena;
    vg::ir::Module module;
    vg::core::PointerRef seed{};
    build_chain(arena, &module, &seed);
    const size_t universe = active_count(arena);

    vg::hal::ExecutionPlan plan;
    plan.capabilities = metal->capabilities();
    plan.module = module;
    plan.published = true;
    plan.discovery_seeds = {seed};
    plan.requested_certificate_mode = vg::core::AccessCertificateMode::DiscoverThenLease;

    vg::hal::CompiledPlan compiled;
    std::string error;
    if (!metal->compile(plan, &compiled, &error)) {
      std::cerr << "discovery: metal compile failed: " << error << "\n";
      return 1;
    }
    vg::hal::Submission submitted;
    if (!metal->submit(compiled, arena, &submitted, &error)) {
      std::cerr << "discovery: metal submit failed: " << error << "\n";
      return 1;
    }
    if (!submitted.result.ok) {
      std::cerr << "discovery: metal execution failed: " << submitted.result.message << "\n";
      return 1;
    }
    if (!require_discovery(submitted, universe, "discovery.metal")) return 1;
  }

  // Reference: submit() itself calls run_discovery_stage when seeds are set.
  {
    vg::core::Arena arena;
    vg::ir::Module module;
    vg::core::PointerRef seed{};
    build_chain(arena, &module, &seed);
    const size_t universe = active_count(arena);

    vg::hal::ExecutionPlan plan;
    plan.capabilities = reference->capabilities();
    plan.module = module;
    plan.published = true;
    plan.discovery_seeds = {seed};
    plan.requested_certificate_mode = vg::core::AccessCertificateMode::DiscoverThenLease;

    vg::hal::CompiledPlan compiled;
    std::string error;
    if (!reference->compile(plan, &compiled, &error)) {
      std::cerr << "discovery: reference compile failed: " << error << "\n";
      return 1;
    }
    vg::hal::Submission submission;
    if (!reference->submit(compiled, arena, &submission, &error)) {
      std::cerr << "discovery: reference submit failed: " << error << "\n";
      return 1;
    }
    if (!submission.result.ok) {
      std::cerr << "discovery: reference execution failed: " << submission.result.message << "\n";
      return 1;
    }
    if (!require_discovery(submission, universe, "discovery.reference")) return 1;
  }

  // Forged witness: a lease naming an allocation the walk never reached.
  {
    vg::core::Arena arena;
    vg::ir::Module module;
    vg::core::PointerRef seed{};
    build_chain(arena, &module, &seed);
    std::string error;
    vg::core::DiscoveryResult walked;
    if (!vg::core::discover_reachable(arena, {seed}, &walked, &error)) {
      std::cerr << "discovery: forged-witness setup walk failed: " << error << "\n";
      return 1;
    }
    vg::core::PointerRef extra{};
    for (const auto& [id, allocation] : arena.allocations()) {
      (void)id;
      if (allocation.state != vg::core::ObjectState::Active) continue;
      bool reached = false;
      for (const auto& ref : walked.reachable) {
        if (ref.allocation == allocation.id && ref.generation == allocation.generation) reached = true;
      }
      if (reached) continue;
      extra = {allocation.id, allocation.generation};
      break;
    }
    if (extra.allocation == 0) {
      std::cerr << "discovery: forged-witness setup found no unreachable allocation\n";
      return 1;
    }
    for (const auto& ref : walked.reachable) {
      if (ref.allocation == extra.allocation && ref.generation == extra.generation) {
        std::cerr << "discovery: forged-witness extra is already reachable\n";
        return 1;
      }
    }

    vg::hal::ExecutionPlan plan;
    plan.capabilities = reference->capabilities();
    plan.module = module;
    plan.published = true;
    plan.discovery_seeds = {seed};
    plan.working_set_lease = vg::core::WorkingSetLease{};
    plan.working_set_lease->allocations.push_back(extra);
    plan.working_set_lease->complete = true;

    vg::hal::Submission submission;
    if (vg::hal::run_discovery_stage(plan, arena, &submission, &error)) {
      std::cerr << "discovery: forged witness lease was accepted\n";
      return 1;
    }
    std::cout << "discovery.forged: refused (" << error << ")\n";
  }

  std::cout << "discovery: ok\n";
  return 0;
}
