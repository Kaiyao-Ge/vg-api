#include "fixture.h"

namespace vg::tests::metal {

// Explicit semantic-boundary negative, not an alternate execution fixture.
bool check_compiled_plan_tampering(vg::metal::DeviceHal* metal_device, vg::core::Arena& arena,
                                   const vg::core::Allocation& target,
                                   const vg::hal::CompiledPlan& compiled, std::string& error) {
    auto untouched = [&]() {
      const auto* allocation = arena.lookup(vg::core::PointerRef{target.id, target.generation});
      return allocation != nullptr &&
             std::ranges::all_of(allocation->bytes, [](uint8_t byte) { return byte == 0; });
    };

    auto bad_package = compiled;
    bad_package.per_node_packages[0].package->canonical_ir_hash = "tampered-package-hash";
    vg::hal::Submission package_submission;
    error.clear();
    const bool package_accepted = metal_device->submit(bad_package, arena, &package_submission, &error);
    if ((package_accepted && package_submission.result.ok) || !untouched()) {
      std::cerr << "effect-dag: tampered package reached execution\n";
      return false;
    }

    auto bad_order = compiled;
    bad_order.plan.task_order[0] = 1;
    vg::hal::Submission order_submission;
    error.clear();
    if (metal_device->submit(bad_order, arena, &order_submission, &error) || !untouched()) {
      std::cerr << "effect-dag: tampered task order reached execution\n";
      return false;
    }
    std::cout << "effect-dag: package/order tamper rejected before execution\n";
  return true;
}

}  // namespace vg::tests::metal
