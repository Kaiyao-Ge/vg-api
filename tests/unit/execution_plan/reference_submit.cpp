#include "fixture.h"

namespace vg::tests::execution_plan {

void test_reference_multi_node_runtime_pointer_fault_preserves_prefix() {
  vg::core::Arena arena;
  auto& first_output = arena.allocate(16);
  auto& pointer_root = arena.allocate(16);
  auto& pointer_target = arena.allocate(16);
  const auto target_ref = vg::core::PointerRef{pointer_target.id, pointer_target.generation};
  std::memcpy(pointer_root.bytes.data(), &target_ref.allocation, sizeof(target_ref.allocation));
  std::memcpy(pointer_root.bytes.data() + sizeof(target_ref.allocation), &target_ref.generation,
              sizeof(target_ref.generation));

  auto pointer_module = canonical_module(pointer_root.id);
  pointer_module.instructions = {{"load_ref", pointer_root.id, 0, 12, 0, pointer_root.generation, 0, 0, ""},
                                 {"store_via", pointer_target.id, 0, 4, 9, pointer_target.generation, 0, 1, ""}};
  pointer_module.declared_effects = {{pointer_root.id, 0, 12, vg::ir::Access::Read, 0}};
  pointer_module.declared_pointer_edges = {{pointer_root.id, 0, pointer_target.id}};
  pointer_module.canonical_json = vg::ir::serialize_module(pointer_module);
  pointer_module.hash = vg::ir::sha256_hex(pointer_module.canonical_json);

  vg::test_support::MultiNodePlanFixture fixture;
  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(vg::test_support::assemble_multi_node_plan(
      arena, {canonical_module(first_output.id), pointer_module},
      {vg::test_support::compute_task(first_output.id, first_output.generation),
       vg::test_support::compute_task(pointer_root.id, pointer_root.generation)},
      {{0, 1}}, &fixture, &plan, &error));
  auto device = vg::reference::make_device_hal();
  vg::hal::CompiledPlan compiled;
  CHECK(device->compile(plan, &compiled, &error));

  const uint32_t stale_generation = target_ref.generation + 1;
  std::memcpy(pointer_root.bytes.data() + sizeof(target_ref.allocation), &stale_generation,
              sizeof(stale_generation));
  vg::hal::Submission submission;
  CHECK(device->submit(compiled, arena, &submission, &error));
  CHECK(!submission.result.ok);
  CHECK(!submission.result.outputs_valid);
  CHECK(submission.result.poison == vg::core::PoisonState::PartiallyProduced);
  CHECK(submission.result.fault.task_index == 1);
  CHECK(!submission.result.fault.code.empty());
  CHECK(!submission.result.fault.message.empty());
  // The merged prefix retains Task 0's successful store as well as both
  // accesses observed by Task 1 before store_via rejects the stale ref.
  const auto matches_effect = [](const vg::ir::Effect& effect, uint64_t allocation,
                                 uint64_t offset, uint64_t size, vg::ir::Access access) {
    return effect.allocation == allocation && effect.offset == offset && effect.size == size &&
           effect.access == access && effect.representation_epoch == 0;
  };
  CHECK(submission.result.trace.size() == 3);
  CHECK(matches_effect(submission.result.trace[0], first_output.id, 0, 4, vg::ir::Access::Write));
  CHECK(matches_effect(submission.result.trace[1], pointer_root.id, 0, 12, vg::ir::Access::Read));
  CHECK(matches_effect(submission.result.trace[2], pointer_target.id, 0, 4, vg::ir::Access::Write));
  const auto& witness = submission.result.witness.entries();
  CHECK(witness.size() == 3);
  CHECK(witness[0].instruction_index == 0 &&
        matches_effect(witness[0].effect, first_output.id, 0, 4, vg::ir::Access::Write));
  CHECK(witness[1].instruction_index == 0 &&
        matches_effect(witness[1].effect, pointer_root.id, 0, 12, vg::ir::Access::Read));
  CHECK(witness[2].instruction_index == 1 &&
        matches_effect(witness[2].effect, pointer_target.id, 0, 4, vg::ir::Access::Write));
  CHECK(first_output.bytes[0] == 7);
  CHECK(std::all_of(pointer_target.bytes.begin(), pointer_target.bytes.end(),
                    [](uint8_t byte) { return byte == 0; }));
}

void test_reference_submit_releases_holds_on_success_and_repeat() {
  vg::core::Arena arena;
  const auto& root = arena.allocate(32);
  const uint64_t root_id = root.id;
  const uint32_t root_generation = root.generation;
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(vg::test_support::assemble_single_node_plan(
      arena, canonical_module(root_id),
      {vg::test_support::compute_task(root_id, root_generation)}, &fixture, &plan, &error));
  auto device = vg::reference::make_device_hal();
  CHECK(device != nullptr);
  vg::hal::CompiledPlan compiled;
  CHECK(device->compile(plan, &compiled, &error));
  for (int attempt = 0; attempt < 2; ++attempt) {
    vg::hal::Submission submission;
    CHECK(device->submit(compiled, arena, &submission, &error));
    CHECK(submission.result.ok);
    CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 0);
  }

  vg::test_support::AssemblyOptions failed_options;
  failed_options.timeline_wait = 1;  // fresh Reference timeline is still 0
  failed_options.timeline_signal = 2;
  vg::test_support::AssembledPlanFixture failed_fixture;
  vg::core::ExecutionPlan failed_plan;
  CHECK(vg::test_support::assemble_single_node_plan(
      arena, canonical_module(root_id),
      {vg::test_support::compute_task(root_id, root_generation)},
      &failed_fixture, &failed_plan, &error, failed_options));
  vg::hal::CompiledPlan failed_compiled;
  CHECK(device->compile(failed_plan, &failed_compiled, &error));
  vg::hal::Submission failed_submission;
  CHECK(device->submit(failed_compiled, arena, &failed_submission, &error));
  CHECK(!failed_submission.result.ok);
  CHECK(arena.lookup(vg::core::PointerRef{root_id, root_generation})->in_flight == 0);
}

}  // namespace vg::tests::execution_plan
