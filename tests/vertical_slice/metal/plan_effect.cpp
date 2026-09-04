#include "fixture.h"

namespace vg::tests::metal {

struct StoreWord {
  uint64_t offset{};
  int64_t value{};
};
struct WordAt {
  uint64_t offset{};
  uint32_t pattern{};
};

vg::ir::Instruction make_store_instruction(const vg::core::Allocation& allocation, StoreWord word) {
  vg::ir::Instruction instruction;
  instruction.op = "store";
  instruction.allocation = allocation.id;
  instruction.generation = allocation.generation;
  instruction.representation_epoch = allocation.representation_epoch;
  instruction.offset = word.offset;
  instruction.size = 4;
  instruction.value = word.value;
  return instruction;
}

vg::ir::Module make_store_pass(const vg::core::Allocation& allocation, uint64_t offset, int64_t value) {
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test/v1";
  module.instructions.push_back(make_store_instruction(allocation, {.offset = offset, .value = value}));
  module.declared_effects.push_back(
      {allocation.id, offset, 4, vg::ir::Access::Write, allocation.representation_epoch});
  return module;
}

// reference_executor.cpp/compute_package.cpp's store fills every byte of
// [offset, offset+size) with the low byte of `value`, broadcast -- not a
// little-endian encoding of `value`. Mirrors compute_package.cpp's private
// store_word_pattern() so this test can check GPU-written bytes directly.
uint32_t store_word_pattern(int64_t value) {
  const auto low_byte = static_cast<uint32_t>(static_cast<uint8_t>(value));
  return low_byte * 0x01010101u;
}

bool bytes_match_pattern(const std::vector<uint8_t>& bytes, WordAt word) {
  if (word.offset + 4 > bytes.size()) return false;
  uint32_t got = 0;
  std::memcpy(&got, bytes.data() + word.offset, 4);
  return got == word.pattern;
}

// TASK-B14 (E012), updated by MD-4: exercises the legacy diagnostic shape
// classifications end to end, while execution consumes Core's sealed
// component/wave schedule. Metal conservatively submits and host-waits one
// command buffer per Task, so every shape has one compute encoder/wait per
// Task and no MTLFence barrier hidden behind the report. The ForkJoin
// construction is not a "textbook diamond":
// classify_effect_graph_shape's edge-count invariant (structural_edges ==
// 2*(node_count-1)) is only satisfiable for node_count==4 when every node
// pair conflicts, so all 4 passes deliberately write the *same* allocation
// with mutually-conflicting effects and zero explicit dependencies, letting
// seal() generate the full C(4,2)=6-edge transitive closure automatically
// this remains useful coverage for the Core diagnostic classifier, not a
// second backend-local scheduling authority.
bool run_effect_dag(const std::string& root) {
  (void)root;
  auto metal_device = vg::metal::make_device_hal();
  if (metal_device == nullptr) {
    std::cerr << "effect-dag: no Metal device available on this host\n";
    return false;
  }

  auto check_shape = [&](const char* label, std::vector<vg::ir::Module> passes,
                         std::vector<std::pair<uint32_t, uint32_t>> dependencies,
                         const std::vector<uint32_t>& expected_task_order,
                         vg::core::EffectGraphShape expected_shape, uint64_t expected_encoder_count,
                         uint64_t expected_barrier_count, vg::core::Arena& arena,
                         const std::vector<std::pair<uint64_t, uint32_t>>& expect_final_bytes) {
    std::string error;
    const size_t expected_node_count = passes.size();
    std::vector<TaskRecord> tasks;
    tasks.reserve(passes.size());
    for (size_t task_index = 0; task_index < passes.size(); ++task_index) {
      auto task = probe_task(passes[task_index]);
      task.x = static_cast<uint32_t>(task_index + 2);
      task.y = static_cast<uint32_t>(task_index + 3);
      task.z = static_cast<uint32_t>(task_index + 4);
      tasks.push_back(task);
    }
    vg::test_support::MultiNodePlanFixture fixture;
    vg::core::ExecutionPlan plan;
    if (!vg::test_support::assemble_multi_node_plan(arena, std::move(passes), std::move(tasks),
                                                    dependencies, &fixture, &plan, &error)) {
      std::cerr << "effect-dag: " << label << " assembly failed: " << error << "\n";
      return false;
    }
    vg::hal::CompiledPlan compiled;
    if (!metal_device->compile(plan, &compiled, &error)) {
      std::cerr << "effect-dag: " << label << " compile failed: " << error << "\n";
      return false;
    }
    if (compiled.plan.validated_effect_graph_shape != expected_shape) {
      std::cerr << "effect-dag: " << label << " classified as unexpected shape\n";
      return false;
    }
    if (compiled.plan.task_order != expected_task_order) {
      std::cerr << "effect-dag: " << label << " sealed task order mismatch\n";
      return false;
    }
    if (compiled.per_node_packages.size() != expected_node_count) {
      std::cerr << "effect-dag: " << label << " per-Node package count mismatch\n";
      return false;
    }
    uint64_t pipeline_events = 0;
    uint64_t package_events = 0;
    for (const auto& event : compiled.report.events) {
      if (event.operation == "metal_pipeline") {
        if (event.classification != vg::hal::LoweringClass::Direct) {
          std::cerr << "effect-dag: " << label << " first pipeline creation was not reported Direct\n";
          return false;
        }
        pipeline_events += event.count;
      }
      if (event.operation == "node_compute_package") package_events += event.count;
    }
    if (pipeline_events != expected_node_count || package_events != expected_node_count) {
      std::cerr << "effect-dag: " << label << " per-Node lowering report count mismatch\n";
      return false;
    }

    // Compiling the same immutable Nodes again must honestly report cache
    // hits, not another Direct pipeline compilation.
    vg::hal::CompiledPlan cached;
    if (!metal_device->compile(plan, &cached, &error)) {
      std::cerr << "effect-dag: " << label << " cached compile failed: " << error << "\n";
      return false;
    }
    uint64_t cached_pipeline_events = 0;
    for (const auto& event : cached.report.events) {
      if (event.operation != "metal_pipeline") continue;
      if (event.classification != vg::hal::LoweringClass::CachedObject) {
        std::cerr << "effect-dag: " << label << " cached pipeline was not reported as CachedObject\n";
        return false;
      }
      cached_pipeline_events += event.count;
    }
    if (cached_pipeline_events != expected_node_count) {
      std::cerr << "effect-dag: " << label << " cached pipeline report count mismatch\n";
      return false;
    }

    vg::hal::Submission submission;
    if (!metal_device->submit(compiled, arena, &submission, &error)) {
      std::cerr << "effect-dag: " << label << " submit failed: " << error << "\n";
      return false;
    }
    if (!submission.result.ok) {
      std::cerr << "effect-dag: " << label << " execution reported failure: " << submission.result.message << "\n";
      return false;
    }
    if (submission.report.encoder_count != expected_encoder_count) {
      std::cerr << "effect-dag: " << label << " encoder_count mismatch: got " << submission.report.encoder_count
                << ", expected " << expected_encoder_count << "\n";
      return false;
    }
    if (submission.report.barrier_count != expected_barrier_count) {
      std::cerr << "effect-dag: " << label << " barrier_count mismatch: got " << submission.report.barrier_count
                << ", expected " << expected_barrier_count << "\n";
      return false;
    }
    const uint64_t expected_buffers = expected_node_count + 1;  // Tasks plus compute-ring publication.
    if (submission.report.command_buffer_count != expected_buffers ||
        submission.report.queue_wait_count != expected_buffers) {
      std::cerr << "effect-dag: " << label
                << " command-buffer/host-wait count does not match scheduled Tasks plus publication\n";
      return false;
    }
    const auto& dispatches = metal_device->last_node_aware_dispatches();
    if (dispatches.size() != expected_task_order.size()) {
      std::cerr << "effect-dag: " << label << " real dispatch observation count mismatch\n";
      return false;
    }
    std::vector<uint32_t> pipeline_ordinals;
    pipeline_ordinals.reserve(dispatches.size());
    for (size_t encoded_index = 0; encoded_index < dispatches.size(); ++encoded_index) {
      const uint32_t task_index = expected_task_order[encoded_index];
      const auto& task = plan.task_graph.tasks()[task_index];
      const auto& dispatch = dispatches[encoded_index];
      if (dispatch.task_index != task_index || dispatch.node_index != task.node_index ||
          dispatch.node_generation != task.node_generation ||
          dispatch.threadgroups != std::array<uint32_t, 3>{task.x, task.y, task.z}) {
        std::cerr << "effect-dag: " << label
                  << " command encoder did not consume sealed order/NodeRef/x-y-z\n";
        return false;
      }
      pipeline_ordinals.push_back(dispatch.pipeline_ordinal);
    }
    std::sort(pipeline_ordinals.begin(), pipeline_ordinals.end());
    if (std::adjacent_find(pipeline_ordinals.begin(), pipeline_ordinals.end()) !=
        pipeline_ordinals.end()) {
      std::cerr << "effect-dag: " << label << " distinct Nodes unexpectedly shared a pipeline\n";
      return false;
    }
    for (const auto& expectation : expect_final_bytes) {
      const auto* allocation = arena.lookup(vg::core::PointerRef{expectation.first, 1});
      if (allocation == nullptr) {
        std::cerr << "effect-dag: " << label << " missing allocation " << expectation.first << " after submit\n";
        return false;
      }
      if (!bytes_match_pattern(allocation->bytes, {.offset = 0, .pattern = store_word_pattern(expectation.second)})) {
        std::cerr << "effect-dag: " << label << " allocation " << expectation.first
                  << " does not hold the expected final value\n";
        return false;
      }
    }
    std::cout << "effect-dag: " << label << " ok\n";
    return true;
  };

  {
    vg::core::Arena arena;
    const auto& a = arena.allocate(4);
    const auto& b = arena.allocate(4);
    const auto& c = arena.allocate(4);
    std::vector<vg::ir::Module> passes{make_store_pass(a, 0, 10), make_store_pass(b, 0, 11),
                                       make_store_pass(c, 0, 12)};
    if (!check_shape("independent-branches", passes, {}, {0, 1, 2},
                     vg::core::EffectGraphShape::IndependentBranches, 4, 0,
                     arena, {{a.id, 10}, {b.id, 11}, {c.id, 12}}))
      return false;
  }

  {
    vg::core::Arena arena;
    const auto& a = arena.allocate(4);
    const auto& b = arena.allocate(4);
    const auto& c = arena.allocate(4);
    std::vector<vg::ir::Module> passes{make_store_pass(a, 0, 20), make_store_pass(b, 0, 21),
                                       make_store_pass(c, 0, 22)};
    if (!check_shape("linear-chain", passes, {{0, 1}, {1, 2}}, {0, 1, 2},
                     vg::core::EffectGraphShape::LinearChain, 4, 0, arena,
                     {{a.id, 20}, {b.id, 21}, {c.id, 22}}))
      return false;
  }

  {
    vg::core::Arena arena;
    const auto& a = arena.allocate(4);
    std::vector<vg::ir::Module> passes{make_store_pass(a, 0, 30), make_store_pass(a, 0, 31),
                                       make_store_pass(a, 0, 32), make_store_pass(a, 0, 33)};
    // Zero explicit dependencies: seal()'s automatic conflict detection over
    // 4 mutually-conflicting writes to the same allocation is what produces
    // the ForkJoin-classified transitive closure here, not add_dependency().
    if (!check_shape("fork-join", passes, {}, {0, 1, 2, 3},
                     vg::core::EffectGraphShape::ForkJoin, 5, 0, arena, {{a.id, 33}}))
      return false;
  }

  {
    // Storage order is [0,1], while the only explicit dependency seals [1,0].
    // The command encoder observation must follow the latter without asking
    // EffectGraph for a second order.
    vg::core::Arena arena;
    const auto& a = arena.allocate(4);
    const auto& b = arena.allocate(4);
    std::vector<vg::ir::Module> passes{make_store_pass(a, 0, 34), make_store_pass(b, 0, 35)};
    if (!check_shape("reverse-storage-order", passes, {{1, 0}}, {1, 0},
                     vg::core::EffectGraphShape::LinearChain, 3, 0, arena,
                     {{a.id, 34}, {b.id, 35}}))
      return false;
  }

  {
    // Two Tasks reuse one Node/package/pipeline, but each is still a distinct
    // dispatch with its own sealed order and non-trivial shape.
    vg::core::Arena arena;
    const auto module = make_probe_module(arena);
    TaskRecord first = probe_task(module);
    first.x = 7;
    first.y = 3;
    first.z = 2;
    TaskRecord second = probe_task(module);
    second.x = 2;
    second.y = 5;
    second.z = 4;
    const std::vector<std::pair<uint32_t, uint32_t>> dependencies{{1, 0}};
    vg::test_support::AssemblyOptions options;
    options.dependencies = &dependencies;
    vg::core::ExecutionPlan plan;
    std::string error;
    if (!assemble_compute_plan(arena, module, {first, second}, &plan, &error, options)) {
      std::cerr << "effect-dag: same-Node assembly failed: " << error << "\n";
      return false;
    }
    if (plan.task_order != std::vector<uint32_t>{1, 0}) {
      std::cerr << "effect-dag: same-Node reverse order was not sealed\n";
      return false;
    }
    vg::hal::CompiledPlan compiled;
    if (!metal_device->compile(plan, &compiled, &error) || compiled.per_node_packages.size() != 1) {
      std::cerr << "effect-dag: same-Node compile/package count failed: " << error << "\n";
      return false;
    }
    uint64_t package_events = 0;
    for (const auto& event : compiled.report.events)
      if (event.operation == "node_compute_package") package_events += event.count;
    if (package_events != 1) {
      std::cerr << "effect-dag: same-Node package report was not de-duplicated\n";
      return false;
    }
    vg::hal::Submission submission;
    if (!metal_device->submit(compiled, arena, &submission, &error) || !submission.result.ok) {
      std::cerr << "effect-dag: same-Node submit failed: "
                << (error.empty() ? submission.result.message : error) << "\n";
      return false;
    }
    const auto& dispatches = metal_device->last_node_aware_dispatches();
    if (dispatches.size() != 2 || dispatches[0].task_index != 1 || dispatches[1].task_index != 0 ||
        dispatches[0].threadgroups != std::array<uint32_t, 3>{2, 5, 4} ||
        dispatches[1].threadgroups != std::array<uint32_t, 3>{7, 3, 2} ||
        dispatches[0].pipeline_ordinal != dispatches[1].pipeline_ordinal ||
        submission.report.encoder_count != 3 || submission.report.barrier_count != 0 ||
        submission.report.command_buffer_count != 3 || submission.report.queue_wait_count != 3) {
      std::cerr << "effect-dag: same-Node dispatch/order/pipeline/report mismatch\n";
      return false;
    }
    std::cout << "effect-dag: same-Node reuse ok\n";
  }

  {
    // Compiled-package and sealed-order tampering must fail before a Metal
    // command can modify the target allocation.
    vg::core::Arena arena;
    const auto& target = arena.allocate(4);
    const auto module = make_store_pass(target, 0, 36);
    vg::core::ExecutionPlan plan;
    std::string error;
    if (!assemble_compute_plan(arena, module, {probe_task(module)}, &plan, &error)) {
      std::cerr << "effect-dag: tamper plan assembly failed: " << error << "\n";
      return false;
    }
    vg::hal::CompiledPlan compiled;
    if (!metal_device->compile(plan, &compiled, &error)) {
      std::cerr << "effect-dag: tamper plan compile failed: " << error << "\n";
      return false;
    }
    if (!check_compiled_plan_tampering(metal_device.get(), arena, target, compiled, error))
      return false;
  }

  {
    // A textbook four-edge diamond remains outside the legacy diagnostic
    // classifier's three named shapes. MD-4 nevertheless lowers it from the
    // sealed component/wave schedule, proving that the diagnostic shape is no
    // longer a backend execution authority.
    vg::core::Arena arena;
    const auto& a = arena.allocate(4);
    const auto& b = arena.allocate(4);
    const auto& c = arena.allocate(4);
    vg::ir::Module source = make_store_pass(a, 0, 40);
    vg::ir::Module middle1;
    middle1.version = 1;
    middle1.root_schema = "vg.test/v1";
    {
      vg::ir::Instruction load;
      load.op = "load";
      load.allocation = a.id;
      load.generation = a.generation;
      load.representation_epoch = a.representation_epoch;
      load.offset = 0;
      load.size = 4;
      middle1.instructions.push_back(load);
      middle1.declared_effects.push_back({a.id, 0, 4, vg::ir::Access::Read, a.representation_epoch});
    }
    middle1.instructions.push_back(make_store_instruction(b, {.offset = 0, .value = 41}));
    middle1.declared_effects.push_back({b.id, 0, 4, vg::ir::Access::Write, b.representation_epoch});
    vg::ir::Module middle2;
    middle2.version = 1;
    middle2.root_schema = "vg.test/v1";
    {
      vg::ir::Instruction load;
      load.op = "load";
      load.allocation = a.id;
      load.generation = a.generation;
      load.representation_epoch = a.representation_epoch;
      load.offset = 0;
      load.size = 4;
      middle2.instructions.push_back(load);
      middle2.declared_effects.push_back({a.id, 0, 4, vg::ir::Access::Read, a.representation_epoch});
    }
    middle2.instructions.push_back(make_store_instruction(c, {.offset = 0, .value = 42}));
    middle2.declared_effects.push_back({c.id, 0, 4, vg::ir::Access::Write, c.representation_epoch});
    vg::ir::Module join;
    join.version = 1;
    join.root_schema = "vg.test/v1";
    {
      vg::ir::Instruction load_b;
      load_b.op = "load";
      load_b.allocation = b.id;
      load_b.generation = b.generation;
      load_b.representation_epoch = b.representation_epoch;
      load_b.offset = 0;
      load_b.size = 4;
      join.instructions.push_back(load_b);
      join.declared_effects.push_back({b.id, 0, 4, vg::ir::Access::Read, b.representation_epoch});
    }

    std::string error;
    const std::vector<vg::ir::Module> passes{source, middle1, middle2, join};
    const std::vector<std::pair<uint32_t, uint32_t>> dependencies{{0, 1}, {0, 2}, {1, 3}, {2, 3}};
    std::vector<TaskRecord> tasks;
    tasks.reserve(passes.size());
    for (const auto& pass : passes) tasks.push_back(probe_task(pass));
    vg::test_support::MultiNodePlanFixture fixture;
    vg::core::ExecutionPlan plan;
    if (!vg::test_support::assemble_multi_node_plan(arena, passes, std::move(tasks), dependencies,
                                                    &fixture, &plan, &error)) {
      std::cerr << "effect-dag: unsupported-shape assembly failed: " << error << "\n";
      return false;
    }
    vg::hal::CompiledPlan compiled;
    vg::hal::Submission submission;
    if (!metal_device->compile(plan, &compiled, &error) ||
        !metal_device->submit(compiled, arena, &submission, &error) ||
        !submission.result.ok || plan.execution_schedule.components.size() != 1 ||
        plan.execution_schedule.components[0].waves.size() != 3 ||
        submission.report.command_buffer_count != 5 ||
        submission.report.queue_wait_count != 5 ||
        !bytes_match_pattern(a.bytes, {.offset = 0, .pattern = store_word_pattern(40)}) ||
        !bytes_match_pattern(b.bytes, {.offset = 0, .pattern = store_word_pattern(41)}) ||
        !bytes_match_pattern(c.bytes, {.offset = 0, .pattern = store_word_pattern(42)})) {
      std::cerr << "effect-dag: generic sealed schedule lowering failed: "
                << (error.empty() ? submission.result.message : error) << "\n";
      return false;
    }
    std::cout << "effect-dag: generic sealed schedule lowering ok\n";
  }

  std::cout << "effect-dag: ok\n";
  return true;
}

}  // namespace vg::tests::metal
